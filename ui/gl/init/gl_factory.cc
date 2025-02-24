// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/gl/init/gl_factory.h"

#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/trace_event/trace_event.h"
#include "ui/gl/gl_share_group.h"
#include "ui/gl/gl_surface.h"
#include "ui/gl/init/gl_initializer.h"

namespace gl {
namespace init {

namespace {
bool InitializeGLOneOffHelper(bool init_extensions) {
  DCHECK_EQ(kGLImplementationNone, GetGLImplementation());

  std::vector<GLImplementation> allowed_impls = GetAllowedGLImplementations();
  DCHECK(!allowed_impls.empty());

  const base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();

  // The default implementation is always the first one in list.
  GLImplementation impl = allowed_impls[0];
  bool fallback_to_software_gl = false;
  if (cmd->HasSwitch(switches::kOverrideUseSoftwareGLForTests)) {
    impl = GetSoftwareGLImplementation();
  } else if (cmd->HasSwitch(switches::kUseGL)) {
    std::string requested_implementation_name =
        cmd->GetSwitchValueASCII(switches::kUseGL);
    if (requested_implementation_name == "any") {
      fallback_to_software_gl = true;
    } else if ((requested_implementation_name ==
                kGLImplementationSwiftShaderName) ||
               (requested_implementation_name ==
                kGLImplementationSwiftShaderForWebGLName)) {
      impl = kGLImplementationSwiftShaderGL;
    } else if (requested_implementation_name == kGLImplementationANGLEName) {
      impl = kGLImplementationEGLGLES2;
    } else {
      impl = GetNamedGLImplementation(requested_implementation_name);
      if (!base::ContainsValue(allowed_impls, impl)) {
        LOG(ERROR) << "Requested GL implementation is not available.";
        return false;
      }
    }
  }

  bool gpu_service_logging = cmd->HasSwitch(switches::kEnableGPUServiceLogging);
  bool disable_gl_drawing = cmd->HasSwitch(switches::kDisableGLDrawingForTests);

  return InitializeGLOneOffImplementation(impl, fallback_to_software_gl,
                                          gpu_service_logging,
                                          disable_gl_drawing, init_extensions);
}

}  // namespace

bool InitializeGLOneOff() {
  TRACE_EVENT0("gpu,startup", "gl::init::InitializeOneOff");
  return InitializeGLOneOffHelper(true);
}

bool InitializeGLNoExtensionsOneOff() {
  TRACE_EVENT0("gpu,startup", "gl::init::InitializeNoExtensionsOneOff");
  return InitializeGLOneOffHelper(false);
}

bool InitializeGLOneOffImplementation(GLImplementation impl,
                                      bool fallback_to_software_gl,
                                      bool gpu_service_logging,
                                      bool disable_gl_drawing,
                                      bool init_extensions) {
  bool initialized =
      InitializeStaticGLBindings(impl) && InitializeGLOneOffPlatform();
  if (!initialized && fallback_to_software_gl) {
    ShutdownGL();
    initialized = InitializeStaticGLBindings(GetSoftwareGLImplementation()) &&
                  InitializeGLOneOffPlatform();
  }
  if (initialized && init_extensions) {
    initialized = InitializeExtensionSettingsOneOffPlatform();
  }

  if (!initialized)
    ShutdownGL();

  if (initialized) {
    DVLOG(1) << "Using " << GetGLImplementationName(GetGLImplementation())
             << " GL implementation.";
    if (gpu_service_logging)
      InitializeDebugGLBindings();
    if (disable_gl_drawing)
      InitializeNullDrawGLBindings();
  }
  return initialized;
}

void ShutdownGL() {
  ShutdownGLPlatform();

  SetGLImplementation(kGLImplementationNone);
  UnloadGLNativeLibraries();
}

scoped_refptr<GLSurface> CreateOffscreenGLSurface(const gfx::Size& size) {
  return CreateOffscreenGLSurfaceWithFormat(size, GLSurfaceFormat());
}

}  // namespace init
}  // namespace gl
