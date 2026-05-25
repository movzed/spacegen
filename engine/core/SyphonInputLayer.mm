// SyphonInputLayer.mm — ObjC++ implementation.
// Bridges the Syphon Metal client API to our C++ engine. Each frame:
//   render() asks the Syphon client for a new id<MTLTexture>; we cache
//   the bare pointer (Metal-cpp is bit-compatible with the ObjC id).

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <Syphon/Syphon.h>

#include "SyphonInputLayer.h"
#include "../backends/metal/MetalRenderer.h"   // for MTL::Device access via context

#include "imgui.h"

#include <cstdio>

namespace spacegen {

// ---- ObjC wrapper -------------------------------------------------------

}  // namespace spacegen (close briefly so the ObjC class is at file scope)

@interface SyphonInputImpl : NSObject {
@public
    SyphonMetalClient* client;       // current connection (may be nil)
    NSDictionary*      serverDesc;   // descriptor we connected to
    id<MTLTexture>     latestTex;    // last frame received
}
- (instancetype) init;
- (void)         dealloc;
- (NSArray*)     listServers;
- (void)         connectToServer:(NSDictionary*)server
                            device:(id<MTLDevice>)device;
- (void)         disconnect;
- (void)         pullLatestFrame;
@end

@implementation SyphonInputImpl
- (instancetype) init {
    self = [super init];
    if (self) {
        client = nil;
        serverDesc = nil;
        latestTex = nil;
    }
    return self;
}
- (void) dealloc {
    [self disconnect];
}
- (NSArray*) listServers {
    return [[SyphonServerDirectory sharedDirectory] servers] ?: @[];
}
- (void) connectToServer:(NSDictionary*)server device:(id<MTLDevice>)device {
    [self disconnect];
    if (!server || !device) return;
    client = [[SyphonMetalClient alloc]
              initWithServerDescription:server
                                 device:device
                                options:nil
                          newFrameHandler:nil];
    serverDesc = server;
}
- (void) disconnect {
    if (client) {
        [client stop];
        client = nil;
    }
    serverDesc = nil;
    latestTex = nil;
}
- (void) pullLatestFrame {
    if (!client) return;
    id<MTLTexture> t = [client newFrameImage];
    if (t) latestTex = t;
}
@end

namespace spacegen {

// ---- Public C++ shell ---------------------------------------------------

SyphonInputLayer::SyphonInputLayer() {
    name      = "Syphon Input";
    blendMode = BlendMode::Normal;
    colorTag  = glm::vec3(0.95f, 0.55f, 0.95f);  // magenta — video
    SyphonInputImpl* impl = [[SyphonInputImpl alloc] init];
    impl_ = (__bridge_retained void*)impl;
}

SyphonInputLayer::~SyphonInputLayer() {
    if (impl_) {
        SyphonInputImpl* impl = (__bridge_transfer SyphonInputImpl*)impl_;
        [impl disconnect];
        impl_ = nullptr;
    }
}

void SyphonInputLayer::render(RenderContext& ctx) {
    if (!impl_) return;
    SyphonInputImpl* impl = (__bridge SyphonInputImpl*)impl_;
    [impl pullLatestFrame];
    (void)ctx;
}

MTL::Texture* SyphonInputLayer::currentTexture() const {
    if (!impl_) return nullptr;
    SyphonInputImpl* impl = (__bridge SyphonInputImpl*)impl_;
    return (__bridge MTL::Texture*)(impl->latestTex);
}

std::string SyphonInputLayer::currentServerName() const {
    if (!impl_) return {};
    SyphonInputImpl* impl = (__bridge SyphonInputImpl*)impl_;
    if (!impl->serverDesc) return {};
    NSString* an = impl->serverDesc[SyphonServerDescriptionAppNameKey];
    NSString* n  = impl->serverDesc[SyphonServerDescriptionNameKey];
    std::string out;
    if (an) out += [an UTF8String];
    if (n && [n length] > 0) {
        if (!out.empty()) out += " — ";
        out += [n UTF8String];
    }
    return out;
}

void SyphonInputLayer::drawInspector() {
    if (!impl_) return;
    SyphonInputImpl* impl = (__bridge SyphonInputImpl*)impl_;

    ImGui::TextDisabled("Live video → mapped onto the structure's UVs");
    ImGui::SliderFloat("Mix##sy",   &mix, 0.0f, 1.0f);
    ImGui::ColorEdit3 ("Tint##sy",  &tint[0],
                       ImGuiColorEditFlags_PickerHueWheel
                       | ImGuiColorEditFlags_Float);
    ImGui::Checkbox("Flip Y##syflip", &flipY);

    ImGui::Separator();

    ImGui::TextDisabled("UV sampling: hybrid auto");
    ImGui::TextWrapped("Where the Blender unwrap is valid (PRT_UVW) the "
                        "shader samples by UV0 — preserving the artist's "
                        "intent on the mask. Where UV0 is degenerate, the "
                        "shader falls back to UV1 (xatlas atlas).");
    ImGui::TextDisabled("Generate the atlas from the UV Analysis panel");
    ImGui::TextDisabled("if you see solid-color faces on flat surfaces.");

    ImGui::Separator();
    NSArray* servers = [impl listServers];
    if ([servers count] == 0) {
        ImGui::TextDisabled("No Syphon servers detected.");
        ImGui::TextDisabled("Open Resolume / MadMapper / etc. and");
        ImGui::TextDisabled("enable Syphon output.");
    } else {
        ImGui::TextDisabled("Available Syphon servers:");
        for (NSDictionary* s in servers) {
            NSString* an = s[SyphonServerDescriptionAppNameKey];
            NSString* nm = s[SyphonServerDescriptionNameKey];
            char label[160];
            std::snprintf(label, sizeof(label), "%s — %s",
                          an ? [an UTF8String] : "?",
                          nm && [nm length] > 0 ? [nm UTF8String] : "(default)");
            bool isCurrent = (impl->serverDesc != nil
                              && [s isEqualToDictionary:impl->serverDesc]);
            if (ImGui::Selectable(label, isCurrent)) {
                // Need a Metal device. Use the system default — Syphon's
                // MetalClient does not require the EXACT same device as
                // the server (IOSurface is shared cross-device).
                auto dev = MTLCreateSystemDefaultDevice();
                [impl connectToServer:s device:dev];
            }
        }
        if (impl->serverDesc != nil) {
            ImGui::Separator();
            std::string nm = currentServerName();
            ImGui::Text("Connected: %s", nm.c_str());
            if (ImGui::Button("Disconnect##sy")) {
                [impl disconnect];
            }
        }
    }

    // Show current frame info if available.
    MTL::Texture* tex = currentTexture();
    if (tex) {
        ImGui::TextDisabled("Frame: %lu x %lu",
                            (unsigned long)tex->width(),
                            (unsigned long)tex->height());
    }
}

} // namespace spacegen
