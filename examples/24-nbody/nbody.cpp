/*
* Copyright 2014 Stanlo Slasinski. All rights reserved.
* License: https://github.com/bkaradzic/bgfx#license-bsd-2-clause
*/

#include "common.h"
#include "bgfx_utils.h"
#include "imgui/imgui.h"
#include "camera.h"
#include <bgfx/bgfx.h>

struct ParamsData
{
	float   timeStep;
	int32_t dispatchSize;
	float   gravity;
	float   damping;
	float   particleIntensity;
	float   particleSize;
	int32_t baseSeed;
	float   particlePower;
	float   initialSpeed;
	int32_t initialShape;
	float   maxAccel;
};

void initializeParams(int32_t _mode, ParamsData* _params)
{
	switch(_mode)
	{
	case 0:
		_params->timeStep          = 0.0067f;
		_params->dispatchSize      = 32;
		_params->gravity           = 0.069f;
		_params->damping           = 0.0f;
		_params->particleIntensity = 0.35f;
		_params->particleSize      = 0.925f;
		_params->baseSeed          = 0;
		_params->particlePower     = 5.0f;
		_params->initialSpeed      = 122.6f;
		_params->initialShape      = 0;
		_params->maxAccel          = 30.0;
		break;

	case 1:
		_params->timeStep          = 0.0157f;
		_params->dispatchSize      = 32;
		_params->gravity           = 0.109f;
		_params->damping           = 0.25f;
		_params->particleIntensity = 0.64f;
		_params->particleSize      = 0.279f;
		_params->baseSeed          = 57;
		_params->particlePower     = 3.5f;
		_params->initialSpeed      = 3.2f;
		_params->initialShape      = 1;
		_params->maxAccel          = 100.0;
		break;

	case 2:
		_params->timeStep          = 0.02f;
		_params->dispatchSize      = 32;
		_params->gravity           = 0.24f;
		_params->damping           = 0.12f;
		_params->particleIntensity = 1.0f;
		_params->particleSize      = 1.0f;
		_params->baseSeed          = 23;
		_params->particlePower     = 4.0f;
		_params->initialSpeed      = 31.1f;
		_params->initialShape      = 2;
		_params->maxAccel          = 39.29f;
		break;

	case 3:
		_params->timeStep          = 0.0118f;
		_params->dispatchSize      = 32;
		_params->gravity           = 0.141f;
		_params->damping           = 1.0f;
		_params->particleIntensity = 0.64f;
		_params->particleSize      = 0.28f;
		_params->baseSeed          = 60;
		_params->particlePower     = 1.97f;
		_params->initialSpeed      = 69.7f;
		_params->initialShape      = 3;
		_params->maxAccel          = 3.21f;
		break;
	}
}

static const float s_quadVertices[] =
{
	 1.0f,  1.0f,
	-1.0f,  1.0f,
	-1.0f, -1.0f,
	 1.0f, -1.0f,
};

static const uint16_t s_quadIndices[] = { 0, 1, 2, 2, 3, 0, };

const uint32_t kThreadGroupUpdateSize = 512;
const uint32_t kMaxParticleCount      = 32 * 1024;

class ExampleNbody : public entry::AppI
{
	void init(int _argc, char** _argv) BX_OVERRIDE
	{
		Args args(_argc, _argv);

		m_width  = 1280;
		m_height = 720;
		m_debug  = BGFX_DEBUG_TEXT;
		m_reset  = BGFX_RESET_VSYNC;

		bgfx::init(args.m_type, args.m_pciId);
		bgfx::reset(m_width, m_height, m_reset);

		// Enable debug text.
		bgfx::setDebug(m_debug);

		// Set view 0 clear state.
		bgfx::setViewClear(0
			, BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH
			, 0x303030ff
			, 1.0f
			, 0
			);

		const bgfx::Caps* caps = bgfx::getCaps();
		const bool computeSupported  = !!(caps->supported & BGFX_CAPS_COMPUTE);
		const bool indirectSupported = !!(caps->supported & BGFX_CAPS_DRAW_INDIRECT);

		if (computeSupported)
		{
			// Imgui.
			imguiCreate();

			bgfx::VertexDecl quadVertexDecl;
			quadVertexDecl.begin()
				.add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
				.end();

			// Create static vertex buffer.
			m_vbh = bgfx::createVertexBuffer(
				// Static data can be passed with bgfx::makeRef
				bgfx::makeRef(s_quadVertices, sizeof(s_quadVertices) )
				, quadVertexDecl
				);

			// Create static index buffer.
			m_ibh = bgfx::createIndexBuffer(
				// Static data can be passed with bgfx::makeRef
				bgfx::makeRef(s_quadIndices, sizeof(s_quadIndices) )
				);

			// Create particle program from shaders.
			m_particleProgram = loadProgram("vs_particle", "fs_particle");

			// Setup compute buffers
			bgfx::VertexDecl computeVertexDecl;
			computeVertexDecl.begin()
				.add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float)
				.end();

			m_currPositionBuffer0 = bgfx::createDynamicVertexBuffer(1 << 15, computeVertexDecl, BGFX_BUFFER_COMPUTE_READ_WRITE);
			m_currPositionBuffer1 = bgfx::createDynamicVertexBuffer(1 << 15, computeVertexDecl, BGFX_BUFFER_COMPUTE_READ_WRITE);
			m_prevPositionBuffer0 = bgfx::createDynamicVertexBuffer(1 << 15, computeVertexDecl, BGFX_BUFFER_COMPUTE_READ_WRITE);
			m_prevPositionBuffer1 = bgfx::createDynamicVertexBuffer(1 << 15, computeVertexDecl, BGFX_BUFFER_COMPUTE_READ_WRITE);

			u_params = bgfx::createUniform("u_params", bgfx::UniformType::Vec4, 3);

			m_initInstancesProgram   = bgfx::createProgram(loadShader("cs_init_instances"), true);
			m_updateInstancesProgram = bgfx::createProgram(loadShader("cs_update_instances"), true);

			m_indirectProgram = BGFX_INVALID_HANDLE;
			m_indirectBuffer  = BGFX_INVALID_HANDLE;

			if (indirectSupported)
			{
				m_indirectProgram = bgfx::createProgram(loadShader("cs_indirect"), true);
				m_indirectBuffer  = bgfx::createIndirectBuffer(2);
			}

			initializeParams(0, &m_paramsData);

			bgfx::setUniform(u_params, &m_paramsData, 3);
			bgfx::setBuffer(0, m_prevPositionBuffer0, bgfx::Access::Write);
			bgfx::setBuffer(1, m_currPositionBuffer0, bgfx::Access::Write);
			bgfx::dispatch(0, m_initInstancesProgram, kMaxParticleCount / kThreadGroupUpdateSize, 1, 1);

			float initialPos[3] = { 0.0f, 0.0f, -45.0f };
			cameraCreate();
			cameraSetPosition(initialPos);
			cameraSetVerticalAngle(0.0f);

			m_useIndirect = false;

			m_timeOffset = bx::getHPCounter();
		}
	}

	virtual int shutdown() BX_OVERRIDE
	{
		// Cleanup.
		cameraDestroy();
		imguiDestroy();

		if (bgfx::isValid(m_indirectProgram) )
		{
			bgfx::destroyProgram(m_indirectProgram);
			bgfx::destroyIndirectBuffer(m_indirectBuffer);
		}

		bgfx::destroyUniform(u_params);
		bgfx::destroyDynamicVertexBuffer(m_currPositionBuffer0);
		bgfx::destroyDynamicVertexBuffer(m_currPositionBuffer1);
		bgfx::destroyDynamicVertexBuffer(m_prevPositionBuffer0);
		bgfx::destroyDynamicVertexBuffer(m_prevPositionBuffer1);
		bgfx::destroyProgram(m_updateInstancesProgram);
		bgfx::destroyProgram(m_initInstancesProgram);
		bgfx::destroyIndexBuffer(m_ibh);
		bgfx::destroyVertexBuffer(m_vbh);
		bgfx::destroyProgram(m_particleProgram);

		// Shutdown bgfx.
		bgfx::shutdown();

		return 0;
	}

	bool update() BX_OVERRIDE
	{
		entry::MouseState mouseState;
		if (!entry::processEvents(m_width, m_height, m_debug, m_reset, &mouseState) )
		{
			const bgfx::Caps* caps = bgfx::getCaps();
			const bool computeSupported  = !!(caps->supported & BGFX_CAPS_COMPUTE);
			const bool indirectSupported = !!(caps->supported & BGFX_CAPS_DRAW_INDIRECT);

			int64_t now = bx::getHPCounter();
			float time = (float)( (now - m_timeOffset)/double(bx::getHPFrequency() ) );
			static int64_t last = now;
			const int64_t frameTime = now - last;
			last = now;
			const double freq = double(bx::getHPFrequency() );
			const float deltaTime = float(frameTime/freq);

			// Set view 0 default viewport.
			bgfx::setViewRect(0, 0, 0, uint16_t(m_width), uint16_t(m_height) );

			if (computeSupported)
			{
				// Use debug font to print information about this example.
				bgfx::dbgTextClear();
				bgfx::dbgTextPrintf(0, 1, 0x4f, "bgfx/examples/24-nbody");
				bgfx::dbgTextPrintf(0, 2, 0x6f, "Description: N-body simulation with compute shaders using buffers.");

				imguiBeginFrame(mouseState.m_mx
					, mouseState.m_my
					, (mouseState.m_buttons[entry::MouseButton::Left  ] ? IMGUI_MBUT_LEFT   : 0)
					| (mouseState.m_buttons[entry::MouseButton::Right ] ? IMGUI_MBUT_RIGHT  : 0)
					| (mouseState.m_buttons[entry::MouseButton::Middle] ? IMGUI_MBUT_MIDDLE : 0)
					, mouseState.m_mz
					, uint16_t(m_width)
					, uint16_t(m_height)
					);
				static int32_t scrollArea = 0;
				imguiBeginScrollArea("Settings", m_width - m_width / 4 - 10, 10, m_width / 4, 500, &scrollArea);
				imguiSlider("Random seed", m_paramsData.baseSeed, 0, 100);
				int32_t shape = imguiChoose(m_paramsData.initialShape, "Point", "Sphere", "Box", "Donut");
				imguiSlider("Initial speed", m_paramsData.initialSpeed, 0.0f, 300.0f, 0.1f);
				bool defaults = imguiButton("Reset");
				imguiSeparatorLine();
				imguiSlider("Particle count (x512)", m_paramsData.dispatchSize, 1, 64);
				imguiSlider("Gravity", m_paramsData.gravity, 0.0f, 0.3f, 0.001f);
				imguiSlider("Damping", m_paramsData.damping, 0.0f, 1.0f, 0.01f);
				imguiSlider("Max acceleration", m_paramsData.maxAccel, 0.0f, 100.0f, 0.01f);
				imguiSlider("Time step", m_paramsData.timeStep, 0.0f, 0.02f, 0.0001f);
				imguiSeparatorLine();
				imguiSlider("Particle intensity", m_paramsData.particleIntensity, 0.0f, 1.0f, 0.001f);
				imguiSlider("Particle size", m_paramsData.particleSize, 0.0f, 1.0f, 0.001f);
				imguiSlider("Particle power", m_paramsData.particlePower, 0.001f, 16.0f, 0.01f);
				imguiSeparatorLine();
				if (imguiCheck("Use draw/dispatch indirect", m_useIndirect, indirectSupported) )
				{
					m_useIndirect = !m_useIndirect;
				}
				imguiEndScrollArea();
				imguiEndFrame();

				// Modify parameters and reset if shape is changed
				if (shape != m_paramsData.initialShape)
				{
					defaults = true;
					initializeParams(shape, &m_paramsData);
				}

				if (defaults)
				{
					bgfx::setBuffer(0, m_prevPositionBuffer0, bgfx::Access::Write);
					bgfx::setBuffer(1, m_currPositionBuffer0, bgfx::Access::Write);
					bgfx::setUniform(u_params, &m_paramsData, 3);
					bgfx::dispatch(0, m_initInstancesProgram, kMaxParticleCount / kThreadGroupUpdateSize, 1, 1);
				}

				if (m_useIndirect)
				{
					bgfx::setUniform(u_params, &m_paramsData, 3);
					bgfx::setBuffer(0, m_indirectBuffer, bgfx::Access::Write);
					bgfx::dispatch(0, m_indirectProgram);
				}

				bgfx::setBuffer(0, m_prevPositionBuffer0, bgfx::Access::Read);
				bgfx::setBuffer(1, m_currPositionBuffer0, bgfx::Access::Read);
				bgfx::setBuffer(2, m_prevPositionBuffer1, bgfx::Access::Write);
				bgfx::setBuffer(3, m_currPositionBuffer1, bgfx::Access::Write);
				bgfx::setUniform(u_params, &m_paramsData, 3);

				if (m_useIndirect)
				{
					bgfx::dispatch(0, m_updateInstancesProgram, m_indirectBuffer, 1);
				}
				else
				{
					bgfx::dispatch(0, m_updateInstancesProgram, uint16_t(m_paramsData.dispatchSize), 1, 1);
				}

				bx::xchg(m_currPositionBuffer0, m_currPositionBuffer1);
				bx::xchg(m_prevPositionBuffer0, m_prevPositionBuffer1);

				// Update camera.
				cameraUpdate(deltaTime, mouseState);

				float view[16];
				cameraGetViewMtx(view);

				// Set view and projection matrix for view 0.
				const bgfx::HMD* hmd = bgfx::getHMD();
				if (NULL != hmd && 0 != (hmd->flags & BGFX_HMD_RENDERING) )
				{
					float viewHead[16];
					float eye[3] = {};
					bx::mtxQuatTranslationHMD(viewHead, hmd->eye[0].rotation, eye);

					float tmp[16];
					bx::mtxMul(tmp, view, viewHead);
					bgfx::setViewTransform(
						  0
						, tmp
						, hmd->eye[0].projection
						, BGFX_VIEW_STEREO
						, hmd->eye[1].projection
						);

					// Set view 0 default viewport.
					//
					// Use HMD's width/height since HMD's internal frame buffer size
					// might be much larger than window size.
					bgfx::setViewRect(0, 0, 0, hmd->width, hmd->height);
				}
				else
				{
					float proj[16];
					bx::mtxProj(
						  proj
						, 90.0f
						, float(m_width)/float(m_height)
						, 0.1f
						, 10000.0f
						, bgfx::getCaps()->homogeneousDepth
						);
					bgfx::setViewTransform(0, view, proj);

					// Set view 0 default viewport.
					bgfx::setViewRect(0, 0, 0, uint16_t(m_width), uint16_t(m_height) );
				}

				// Set vertex and index buffer.
				bgfx::setVertexBuffer(0, m_vbh);
				bgfx::setIndexBuffer(m_ibh);
				bgfx::setInstanceDataBuffer(m_currPositionBuffer0
					, 0
					, m_paramsData.dispatchSize * kThreadGroupUpdateSize
					);

				// Set render states.
				bgfx::setState(0
					| BGFX_STATE_RGB_WRITE
					| BGFX_STATE_BLEND_ADD
					| BGFX_STATE_DEPTH_TEST_ALWAYS
					);

				// Submit primitive for rendering to view 0.
				if (m_useIndirect)
				{
					bgfx::submit(0, m_particleProgram, m_indirectBuffer, 0);
				}
				else
				{
					bgfx::submit(0, m_particleProgram);
				}
			}
			else
			{
				bgfx::setViewRect(0, 0, 0, uint16_t(m_width), uint16_t(m_height) );

				bgfx::dbgTextClear();
				bgfx::dbgTextPrintf(0, 1, 0x4f, "bgfx/examples/24-nbody");
				bgfx::dbgTextPrintf(0, 2, 0x6f, "Description: N-body simulation with compute shaders using buffers.");

				bool blink = uint32_t(time*3.0f)&1;
				bgfx::dbgTextPrintf(0, 5, blink ? 0x1f : 0x01, " Compute is not supported by GPU. ");

				bgfx::touch(0);
			}

			// Advance to next frame. Rendering thread will be kicked to
			// process submitted rendering primitives.
			bgfx::frame();

			return true;
		}

		return false;
	}

	uint32_t m_width;
	uint32_t m_height;
	uint32_t m_debug;
	uint32_t m_reset;
	bool m_useIndirect;

	ParamsData m_paramsData;

	bgfx::VertexBufferHandle m_vbh;
	bgfx::IndexBufferHandle  m_ibh;
	bgfx::ProgramHandle m_particleProgram;
	bgfx::ProgramHandle m_indirectProgram;
	bgfx::ProgramHandle m_initInstancesProgram;
	bgfx::ProgramHandle m_updateInstancesProgram;
	bgfx::IndirectBufferHandle m_indirectBuffer;
	bgfx::DynamicVertexBufferHandle m_currPositionBuffer0;
	bgfx::DynamicVertexBufferHandle m_currPositionBuffer1;
	bgfx::DynamicVertexBufferHandle m_prevPositionBuffer0;
	bgfx::DynamicVertexBufferHandle m_prevPositionBuffer1;
	bgfx::UniformHandle u_params;

	int64_t m_timeOffset;
};

ENTRY_IMPLEMENT_MAIN(ExampleNbody);
