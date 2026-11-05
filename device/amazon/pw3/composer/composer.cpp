// SPDX-License-Identifier: Apache-2.0
// CPU composition through HWC 1.5. This is not a claim of a hardware GPU.
// Layers are composed in luma, straight into the canvas of the PW3 panel
// (gralloc/PanelOps.h), and only where SurfaceFlinger reports a change.
// Only exact integer 1:1 layers are accepted; all other primary frames use
// GLES and reach the panel through the framebuffer device's post().
#include "BufferFormat.h"
#include "LumaCompose.h"
#include "../gralloc/PanelOps.h"
#include "../gralloc/gralloc_priv.h"
#include <hardware/hwcomposer.h>
#include <hardware/fb.h>
#include <sync/sync.h>
#include <cutils/properties.h>
#include <log/log.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr size_t MaxLayers = 256, MaxRects = 256, MaxDisplays = 32, MaxDamage = 8;
// Private hwc_layer_1_t::flags bit, set by the PW3 SurfaceFlinger on dim surfaces.
constexpr uint32_t DimLayerFlag = 0x40000000;
int64_t now() { timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return int64_t(t.tv_sec)*1000000000+t.tv_nsec; }
struct Device {
    hwc_composer_device_1_t api{};
    const gralloc_module_t* gralloc = nullptr;
    framebuffer_device_t* framebuffer = nullptr;
    const Pw3PanelOps* panel = nullptr;
    pw3::Canvas canvas{};
    int blankFd = -1;
    int64_t period = 33333333;
    std::mutex render;
    pthread_mutex_t eventMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t eventCondition{};
    pthread_t eventThread{};
    const hwc_procs_t* callbacks = nullptr;
    bool stopping = false, vsyncEnabled = false, displayOn = true;
    bool conditionReady = false, threadStarted = false, diagnostic = false;
    pthread_mutex_t workMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t workCondition = PTHREAD_COND_INITIALIZER;
    pthread_t workThread{};
    bool workStarted = false, workStopping = false, workPending = false;
    int workerReady = 0, workResult = 0;
    size_t workDisplay = 0;
    hwc_display_contents_1_t* workList = nullptr;
    // Partial composition is valid only on top of a frame this composer drew
    // from the same layer list.
    bool canvasCurrent = false, useDamage = true;
    // debug.pw3.hwc.verify=1: compose every partial frame completely as well
    // and compare. A difference means some change was not reported as damage.
    bool verify = false;
    std::vector<uint8_t> verifyLuma;
    uint64_t verifiedFrames = 0, verifyFailures = 0;
    size_t lastLayers = 0;
    buffer_handle_t lastHandles[MaxLayers] = {};
    // The screen rectangle of each layer of the previous frame, to tell a
    // window that appeared or went away from one that only drew again.
    pw3::Rect lastFrames[MaxLayers] = {};
    uint64_t cpuFrames = 0, fallbackFrames = 0, failures = 0, skippedOffFrames = 0;
    // Frames with no layer at all, only the surface composer's own target. Not
    // posted (see the worker): the target is cleared black, and the panel is
    // better left showing what it has. Counted apart from the refusals, which
    // are what cost seconds a frame.
    uint64_t emptyFrames = 0;
    uint64_t partialFrames = 0, idleFrames = 0, dimFrames = 0, composeUsTotal = 0, postUsTotal = 0, composedPixels = 0;
    // Areas composed whose bytes all ended as they began: the panel cannot
    // show anything new for them (LumaCompose.h), so they are dropped from the
    // damage. Measured on V52: a launch of the browser and back composes 16
    // frames, 2 to 3 of whose areas end as they began, and a whole-screen area
    // costs the panel 40 ms to convert and compare - about 100 ms saved per
    // launch, against 1.2 ms per frame for the comparison itself.
    // debug.pw3.hwc.skip_unchanged=false turns it off.
    uint64_t unchangedAreas = 0, unchangedPixels = 0, unchangedFrames = 0;
    bool skipUnchanged = false;
    int64_t lastComposeUs = 0, lastPostUs = 0, maxComposeUs = 0, maxPostUs = 0;
    unsigned rejectedReports = 0;
    ~Device() {
        if(threadStarted) {
            pthread_mutex_lock(&eventMutex);stopping=true;pthread_cond_signal(&eventCondition);
            pthread_mutex_unlock(&eventMutex);pthread_join(eventThread,nullptr);
        }
        if(conditionReady)pthread_cond_destroy(&eventCondition);
        pthread_mutex_destroy(&eventMutex);
        if(workStarted) {
            pthread_mutex_lock(&workMutex);workStopping=true;pthread_cond_signal(&workCondition);
            pthread_mutex_unlock(&workMutex);pthread_join(workThread,nullptr);
        }
        pthread_cond_destroy(&workCondition);pthread_mutex_destroy(&workMutex);
        if(framebuffer)framebuffer_close(framebuffer);
        if(blankFd>=0)close(blankFd);
    }
};
Device* device(hwc_composer_device_1_t* d) { return reinterpret_cast<Device*>(d); }
// The buffer's pixel format, for the report of a layer this composer refused;
// -1 when there is no buffer (a dim layer) or it is not one of ours.
int bufferFormat(buffer_handle_t handle) {
    if(!handle||private_handle_t::validate(handle))return -1;
    return static_cast<const private_handle_t*>(handle)->format;
}
bool image(buffer_handle_t handle, pw3::Image* out) {
    if(private_handle_t::validate(handle))return false;
    const auto* h=static_cast<const private_handle_t*>(handle);
    pw3::PixelFormat format;
    if(!pw3::bufferFormat(h->format,&format))return false;
    *out={nullptr,size_t(h->size),h->width,h->height,h->stride,format};
    return pw3::validImage(*out);
}
bool integer(float value,int* out) {
    if(!std::isfinite(value)||value<0||value>4096||std::floor(value)!=value)return false;
    *out=int(value);return true;
}
bool region(const hwc_layer_1_t& in,int width,int height) {
    const auto& region=in.visibleRegionScreen;
    if(region.numRects>MaxRects||(region.numRects&&!region.rects))return false;
    // Android supplies a normalized, disjoint region. Reject overlapping input
    // instead of blending one pixel twice, and bound every coordinate.
    for(size_t i=0;i<region.numRects;++i) {
        const auto& r=region.rects[i];
        if(r.left<0||r.top<0||r.right>width||r.bottom>height||r.right<=r.left||r.bottom<=r.top)return false;
        for(size_t j=0;j<i;++j) {
            const auto& q=region.rects[j];
            if(std::max(r.left,q.left)<std::min(r.right,q.right)&&
               std::max(r.top,q.top)<std::min(r.bottom,q.bottom))return false;
        }
    }
    return true;
}
// Describes one HWC layer for the luma composer; false sends the frame to GLES.
// A dim surface has no buffer: black, weighted by the plane alpha. The PW3
// SurfaceFlinger marks it with DimLayerFlag instead of HWC_SKIP_LAYER
// (frameworks/native patch 0003); only its frame, region and alpha are used.
bool layer(const hwc_layer_1_t& in,pw3::Layer* out,int width,int height) {
    *out={};
    if(in.flags&HWC_SKIP_LAYER)return false;
    const auto& f=in.displayFrame;out->frame={f.left,f.top,f.right,f.bottom};out->alpha=in.planeAlpha;
    static_assert(sizeof(hwc_rect_t)==sizeof(pw3::Rect),"visible rectangles are used in place");
    out->visible=reinterpret_cast<const pw3::Rect*>(in.visibleRegionScreen.rects);
    out->visibleCount=in.visibleRegionScreen.numRects;
    if(in.flags&DimLayerFlag) {
        if(in.handle)return false;
        out->dim=true;out->blend=pw3::Blend::Premultiplied;
        return region(in,width,height)&&pw3::supported(*out,width,height);
    }
    // Flips and quarter turns are composed here (a landscape display turns
    // every layer), and so is a frame shown at a size other than its crop.
    if(!image(in.handle,&out->image))return false;
    out->transform=in.transform;
    const auto& c=in.sourceCropf;
    if(!integer(c.left,&out->crop.left)||!integer(c.top,&out->crop.top)||
       !integer(c.right,&out->crop.right)||!integer(c.bottom,&out->crop.bottom))return false;
    switch(in.blending) {
        case HWC_BLENDING_NONE:out->blend=pw3::Blend::Opaque;break;
        case HWC_BLENDING_PREMULT:out->blend=pw3::Blend::Premultiplied;break;
        case HWC_BLENDING_COVERAGE:out->blend=pw3::Blend::Coverage;break;
        default:return false;
    }
    return region(in,width,height)&&pw3::supported(*out,width,height);
}
bool candidate(Device* d,hwc_display_contents_1_t* list) {
    // A frame with nothing but the target has no layer to take and is counted on
    // its own (empty_frames): the surface composer has already made what there is
    // to show, and there is nothing here to refuse.
    if(!d->panel||!list||list->numHwLayers<2)return false;
    // The two that used to be silent. Every frame that goes to GLES has to be able
    // to say why, or a counter above zero cannot be told from a harmless one: Test
    // V55 showed eight such frames with no line explaining any of them.
    if(list->numHwLayers>MaxLayers||list->hwLayers[list->numHwLayers-1].compositionType!=HWC_FRAMEBUFFER_TARGET) {
        if(d->rejectedReports++<8)
            ALOGW("GLES fallback layers=%zu last_type=%d",(size_t)list->numHwLayers,
                  list->hwLayers[list->numHwLayers-1].compositionType);
        return false;
    }
    for(size_t i=0;i+1<list->numHwLayers;++i) {
        pw3::Layer value{};
        if(!layer(list->hwLayers[i],&value,d->canvas.width,d->canvas.height)) {
            // In every build, not only a debuggable one: a layer this composer
            // cannot take sends the whole frame through SwiftShader, which costs
            // seconds per frame on this CPU (N35), and a report of that needs to
            // say which layer and why. Geometry and format only, at most eight
            // lines per boot. Sampling and the narrow formats are composed here
            // now, so what is left is a skip layer, a buffer no application can
            // ask for (FP16, RAW16), a crop on a fraction of a pixel, or a
            // visible region the surface composer did not make disjoint.
            if(d->rejectedReports++<8) {
                const auto& l=list->hwLayers[i];
                ALOGW("GLES fallback layer=%zu flags=%u transform=%u alpha=%u blending=%u format=%d crop=%.2f,%.2f,%.2f,%.2f frame=%d,%d,%d,%d",
                      i,l.flags,l.transform,l.planeAlpha,l.blending,bufferFormat(l.handle),
                      l.sourceCropf.left,l.sourceCropf.top,l.sourceCropf.right,l.sourceCropf.bottom,
                      l.displayFrame.left,l.displayFrame.top,l.displayFrame.right,l.displayFrame.bottom);
            }
            return false;
        }
    }
    return true;
}
int prepare(hwc_composer_device_1_t* api,size_t count,hwc_display_contents_1_t** lists) {
    if(!lists||count>MaxDisplays)return -EINVAL;
    Device* d=device(api);std::lock_guard<std::mutex> hold(d->render);
    for(size_t display=0;display<count;++display) {
        auto* list=lists[display];if(!list)continue;
        if(list->numHwLayers>MaxLayers)return -EINVAL;
        const bool cpu=display==HWC_DISPLAY_PRIMARY&&candidate(d,list);
        for(size_t i=0;i<list->numHwLayers;++i) {
            auto& l=list->hwLayers[i];
            if(l.compositionType!=HWC_FRAMEBUFFER_TARGET)l.compositionType=cpu?HWC_OVERLAY:HWC_FRAMEBUFFER;
            l.hints=0;
        }
    }
    return 0;
}
int acquire(int& fd) {
    if(fd<0)return 0;
    int rc;do {rc=sync_wait(fd,3000);}while(rc<0&&errno==EINTR);
    int result=rc<0?-errno:0;close(fd);fd=-1;return result;
}
struct Fences {
    hwc_display_contents_1_t* list;
    explicit Fences(hwc_display_contents_1_t* l):list(l) {
        list->retireFenceFd=-1;
        for(size_t i=0;i<list->numHwLayers;++i)list->hwLayers[i].releaseFenceFd=-1;
    }
    ~Fences() {
        for(size_t i=0;i<list->numHwLayers;++i) {
            int& fd=list->hwLayers[i].acquireFenceFd;if(fd>=0){close(fd);fd=-1;}
        }
        if(list->outbufAcquireFenceFd>=0){close(list->outbufAcquireFenceFd);list->outbufAcquireFenceFd=-1;}
    }
};
// The screen area that changed since the previous frame, as at most MaxDamage
// rectangles. Returns false when everything has to be composed.
bool damaged(Device* d,hwc_display_contents_1_t* list,const pw3::Layer* layers,pw3::Rect* damage,size_t* count) {
    const size_t n=list->numHwLayers-1;
    *count=0;
    if(!d->useDamage||!d->canvasCurrent||(list->flags&HWC_GEOMETRY_CHANGED)||n!=d->lastLayers||d->panel->needsFull(d->framebuffer))
        return false;
    const pw3::Rect screen{0,0,d->canvas.width,d->canvas.height};
    const auto add=[&](pw3::Rect r) {
        r=pw3::intersect(r,screen);
        if(pw3::isEmpty(r))return;
        if(*count==MaxDamage) {
            for(size_t i=1;i<MaxDamage;++i)damage[0]=pw3::bounds(damage[0],damage[i]);
            *count=1;
        }
        damage[(*count)++]=r;
    };
    for(size_t i=0;i<n;++i) {
        const auto& in=list->hwLayers[i];
        // A dim surface changes only together with the layer geometry.
        if(layers[i].dim)continue;
        const auto& changed=in.surfaceDamage;
        if(changed.numRects>MaxRects||(changed.numRects&&!changed.rects))return false;
        bool none=changed.numRects>0;
        for(size_t r=0;r<changed.numRects;++r) {
            const auto& c=changed.rects[r];
            if(c.right<=c.left||c.bottom<=c.top)continue;
            none=false;
            add(pw3::onScreen(layers[i],{c.left,c.top,c.right,c.bottom}));
        }
        // No rectangle at all means the whole buffer. A buffer that was
        // replaced without any reported change is not trusted either.
        if(changed.numRects==0||(none&&in.handle!=d->lastHandles[i]))add(layers[i].frame);
    }
    long long total=0;
    for(size_t i=0;i<*count;++i)total+=pw3::area(damage[i]);
    return total*8<pw3::area(screen)*7;
}
// A window appeared or went away: tell the panel where, so that the update
// bringing the new content clears it instead of drawing over what is there
// (Panel::replaced). What identifies a window here is its screen rectangle --
// HWC1 gives layers no identity that survives a frame, and a buffer handle is
// the drawing, not the window -- so a rectangle in one frame's list and not in
// the other's is one that arrived or left. A window that only drew again keeps
// its rectangle and is not reported, which is what keeps typing, scrolling and
// page turns out of this.
//
// Two rectangles that are equal are taken to be the same window even if they
// are not. Being wrong that way costs a ghost that the counter clears later,
// which is where this started; being wrong the other way costs a flash the
// reader did not ask for.
void windowsChanged(Device* d,const pw3::Layer* layers,size_t n) {
    if(!d->panel||n>MaxLayers)return;
    const auto held=[](const pw3::Rect* list,size_t count,const pw3::Rect& r) {
        for(size_t i=0;i<count;++i)
            if(list[i].left==r.left&&list[i].top==r.top&&list[i].right==r.right&&list[i].bottom==r.bottom)
                return true;
        return false;
    };
    pw3::Rect areas[MaxDamage];size_t count=0;
    const auto add=[&](const pw3::Rect& r) {
        if(pw3::isEmpty(r))return;
        if(count==MaxDamage) {
            for(size_t i=1;i<MaxDamage;++i)areas[0]=pw3::bounds(areas[0],areas[i]);
            count=1;
        }
        areas[count++]=r;
    };
    static pw3::Rect now[MaxLayers];
    for(size_t i=0;i<n;++i)now[i]=layers[i].frame;
    for(size_t i=0;i<n;++i)
        if(!held(d->lastFrames,d->lastLayers,now[i]))add(now[i]);
    for(size_t i=0;i<d->lastLayers;++i)
        if(!held(now,n,d->lastFrames[i]))add(d->lastFrames[i]);
    if(count)d->panel->replaced(d->framebuffer,areas,count);
}
int primary(Device* d,hwc_display_contents_1_t* list) {
    if(!list->numHwLayers)return -EINVAL;
    auto& target=list->hwLayers[list->numHwLayers-1];
    bool cpu=d->panel&&list->numHwLayers>1;
    for(size_t i=0;i+1<list->numHwLayers;++i)cpu&=list->hwLayers[i].compositionType==HWC_OVERLAY;
    const int64_t began=now();
    int rc=0;
    if(cpu) {
        if(!candidate(d,list))return -EINVAL;
        const size_t n=list->numHwLayers-1;
        static pw3::Layer layers[MaxLayers];
        unsigned dims=0;
        for(size_t i=0;i<n;++i) {
            auto& in=list->hwLayers[i];
            if(!layer(in,&layers[i],d->canvas.width,d->canvas.height))return -EINVAL;
            if((rc=acquire(in.acquireFenceFd)))return rc;
            dims+=layers[i].dim;
        }
        pw3::Rect damage[MaxDamage];size_t count=0;
        if((d->cpuFrames&31)==0) {
            d->useDamage=property_get_bool("debug.pw3.hwc.damage",true);
            d->verify=property_get_bool("debug.pw3.hwc.verify",false);
            d->skipUnchanged=property_get_bool("debug.pw3.hwc.skip_unchanged",true);
        }
        bool partial=damaged(d,list,layers,damage,&count);
        if(!partial){damage[0]={0,0,d->canvas.width,d->canvas.height};count=1;}
        windowsChanged(d,layers,n);
        d->lastLayers=n;
        for(size_t i=0;i<n;++i)d->lastHandles[i]=list->hwLayers[i].handle;
        for(size_t i=0;i<n;++i)d->lastFrames[i]=layers[i].frame;
        // From here on the canvas no longer matches the previous frame.
        d->canvasCurrent=false;
        // A frame without reported damage is composed only to be verified.
        if(count||(partial&&d->verify)) {
            // The PW3 gralloc keeps every buffer mapped; lock() returns that address.
            size_t locked=0;
            for(;locked<n&&!rc;++locked) {
                auto& l=layers[locked];
                if(l.dim)continue;
                void* address=nullptr;
                rc=d->gralloc->lock(d->gralloc,list->hwLayers[locked].handle,GRALLOC_USAGE_SW_READ_OFTEN,0,0,
                                    l.image.width,l.image.height,&address);
                if(rc){break;}
                l.image.pixels=static_cast<const uint8_t*>(address);
            }
            if(!rc) {
                // An area may be dropped only while the panel knows what it shows.
                // When it does not - a sleep picture was drawn over it, the contrast
                // changed, nothing has been shown yet - the canvas can be unchanged
                // and the screen still wrong, and the repaint must go through.
                const bool shownKnown=!d->panel->needsFull(d->framebuffer);
                size_t kept=0;
                for(size_t r=0;r<count;++r) {
                    const bool changed=pw3::compose(layers,n,d->canvas,damage[r]);
                    d->composedPixels+=uint64_t(pw3::area(damage[r]));
                    if(!changed) {
                        ++d->unchangedAreas;d->unchangedPixels+=uint64_t(pw3::area(damage[r]));
                        if(d->skipUnchanged&&shownKnown)continue;
                    }
                    damage[kept++]=damage[r];
                }
                if(kept!=count) {
                    // Every dropped area composed to the bytes already there, and the
                    // panel shows those bytes: converting them would end in no update.
                    if(!kept)++d->unchangedFrames;
                    count=kept;
                }
            }
            if(!rc&&partial&&d->verify) {
                const pw3::Rect screen{0,0,d->canvas.width,d->canvas.height};
                const size_t pixels=size_t(screen.right)*size_t(screen.bottom);
                d->verifyLuma.resize(pixels);
                pw3::compose(layers,n,pw3::Canvas{d->verifyLuma.data(),screen.right,screen.bottom},screen);
                ++d->verifiedFrames;
                if(std::memcmp(d->verifyLuma.data(),d->canvas.luma,pixels)) {
                    pw3::Rect wrong{0,0,0,0};size_t differing=0;
                    for(int y=0;y<screen.bottom;++y)for(int x=0;x<screen.right;++x) {
                        const size_t at=size_t(y)*size_t(screen.right)+size_t(x);
                        if(d->verifyLuma[at]==d->canvas.luma[at])continue;
                        ++differing;wrong=pw3::bounds(wrong,pw3::Rect{x,y,x+1,y+1});
                    }
                    if(d->verifyFailures++<16)
                        ALOGE("partial composition differs from a complete one: %zu pixels in %d,%d-%d,%d; damage[0]=%d,%d-%d,%d of %zu, layers=%zu",
                              differing,wrong.left,wrong.top,wrong.right,wrong.bottom,damage[0].left,damage[0].top,damage[0].right,damage[0].bottom,count,n);
                    std::memcpy(d->canvas.luma,d->verifyLuma.data(),pixels);
                    damage[0]=screen;count=1;partial=false;
                }
            }
            for(size_t i=0;i<locked;++i)if(!layers[i].dim)d->gralloc->unlock(d->gralloc,list->hwLayers[i].handle);
            if(rc)return rc;
        }
        d->lastComposeUs=(now()-began)/1000;
        const int64_t postBegan=now();
        rc=count?d->panel->present(d->framebuffer,damage,count):0;
        d->lastPostUs=(now()-postBegan)/1000;
        if(!rc) {
            d->canvasCurrent=true;
            ++d->cpuFrames;if(partial)++d->partialFrames;if(!count)++d->idleFrames;if(dims)++d->dimFrames;
        }
    } else if(list->numHwLayers<2) {
        // Nothing but the target: the surface composer's own cleared buffer, and
        // cleared is black (frameworks/native patch 0002 clears with 0,0,0,0).
        // Posting it painted the panel black, twice in every start: from the
        // boot picture the handoff had drawn until the first window -- 85 s of
        // a start that had to recompile an application (2026-11-04, builds g
        // and h, five such frames) -- and again for the 0.3 s between the
        // fallback home going away and the launcher's first frame (build i:
        // FallbackHome destroyed 10:55:16.10, Launcher displayed 16.43), a full
        // black update right before the home screen. An e-ink panel keeps what
        // it shows, and a frame with nothing in it has nothing to replace that
        // with; whatever comes next does. The surface composer sends such a
        // frame once when its last layer goes and then stops composing
        // (SurfaceFlinger_hwc1.cpp, "all layers removed"), so the panel keeps
        // the last window until a new one draws: at boot and when the launcher
        // restarts that is under a second, and a system with no window at all
        // does not otherwise occur (the home is always there). The fence is
        // still taken, as every frame's is, and the frame is counted
        // (empty_frames) so that the dump can say how many were dropped.
        if(target.compositionType!=HWC_FRAMEBUFFER_TARGET||!target.handle)return -EINVAL;
        if((rc=acquire(target.acquireFenceFd)))return rc;
        d->canvasCurrent=false;
        d->lastComposeUs=(now()-began)/1000;d->lastPostUs=0;
        ++d->emptyFrames;
    } else {
        if(target.compositionType!=HWC_FRAMEBUFFER_TARGET||!target.handle)return -EINVAL;
        if((rc=acquire(target.acquireFenceFd)))return rc;
        d->canvasCurrent=false;
        d->lastComposeUs=(now()-began)/1000;
        const int64_t postBegan=now();rc=d->framebuffer->post(d->framebuffer,target.handle);
        d->lastPostUs=(now()-postBegan)/1000;
        if(!rc)++d->fallbackFrames;
    }
    d->composeUsTotal+=uint64_t(d->lastComposeUs);d->postUsTotal+=uint64_t(d->lastPostUs);
    d->maxComposeUs=std::max(d->maxComposeUs,d->lastComposeUs);d->maxPostUs=std::max(d->maxPostUs,d->lastPostUs);
    if(!rc&&d->diagnostic&&cpu&&d->cpuFrames==1)
        ALOGI("CPU luma first complete frame layers=%zu compose_us=%lld post_us=%lld",list->numHwLayers-1,
              (long long)d->lastComposeUs,(long long)d->lastPostUs);
    return rc;
}
struct Mapping {
    Device* d;buffer_handle_t handle;void* address=nullptr;
    Mapping(Device* owner,buffer_handle_t value):d(owner),handle(value) {}
    int lock(int usage,const pw3::Image& shape) {
        return d->gralloc->lock(d->gralloc,handle,usage,0,0,shape.width,shape.height,&address);
    }
    ~Mapping(){if(address)d->gralloc->unlock(d->gralloc,handle);}
};
// Virtual displays remain client-composed. Copy their finished buffer to the
// requested output, honoring both fences; never post it to the physical panel.
int virtualDisplay(Device* d,hwc_display_contents_1_t* list) {
    if(!list->numHwLayers||!list->outbuf)return -EINVAL;
    auto& target=list->hwLayers[list->numHwLayers-1];
    if(target.compositionType!=HWC_FRAMEBUFFER_TARGET)return -EINVAL;
    pw3::Image src{},dst{};if(!image(target.handle,&src)||!image(list->outbuf,&dst)||
        src.width!=dst.width||src.height!=dst.height||src.format!=dst.format)return -ENOTSUP;
    int rc=acquire(target.acquireFenceFd);if(rc)return rc;
    if((rc=acquire(list->outbufAcquireFenceFd)))return rc;
    if(target.handle==list->outbuf)return 0;
    Mapping source(d,target.handle),destination(d,list->outbuf);
    if((rc=source.lock(GRALLOC_USAGE_SW_READ_OFTEN,src)))return rc;
    if((rc=destination.lock(GRALLOC_USAGE_SW_WRITE_OFTEN,dst)))return rc;
    // Both buffers are in the same format, checked above; it is not always
    // four bytes a pixel since the composer took the narrow ones.
    const size_t bytes=size_t(pw3::bytesPerPixel(src.format));
    for(int y=0;y<src.height;++y)
        std::memcpy(static_cast<uint8_t*>(destination.address)+size_t(y)*dst.stride*bytes,
                    static_cast<uint8_t*>(source.address)+size_t(y)*src.stride*bytes,size_t(src.width)*bytes);
    return 0;
}
void* compositionLoop(void* pointer) {
    auto* d=static_cast<Device*>(pointer);
    // Do not run pixel work on SurfaceFlinger's FIFO thread; the calling
    // thread keeps its policy and waits. This worker is CFS at the display
    // priority (-8), so a frame is not held up by the background threads of
    // applications: on Test V32 a frame needing 25 ms of CPU took 69 ms.
    int error=0;
    sched_param parameters{};
    const int rc=pthread_setschedparam(pthread_self(),SCHED_OTHER,&parameters);
    if(rc)error=rc;
    if(!error&&setpriority(PRIO_PROCESS,0,-8)<0)error=errno;
    pthread_mutex_lock(&d->workMutex);d->workerReady=error?-error:1;
    pthread_cond_broadcast(&d->workCondition);
    if(error){pthread_mutex_unlock(&d->workMutex);return nullptr;}
    while(!d->workStopping) {
        while(!d->workStopping&&!d->workPending)pthread_cond_wait(&d->workCondition,&d->workMutex);
        if(d->workStopping)break;
        auto* list=d->workList;const size_t display=d->workDisplay;
        pthread_mutex_unlock(&d->workMutex);
        const int result=display==HWC_DISPLAY_PRIMARY?primary(d,list):virtualDisplay(d,list);
        pthread_mutex_lock(&d->workMutex);d->workResult=result;d->workPending=false;
        pthread_cond_broadcast(&d->workCondition);
    }
    pthread_mutex_unlock(&d->workMutex);return nullptr;
}
int submit(Device* d,size_t display,hwc_display_contents_1_t* list) {
    // present() serializes submissions and retains the list and all fences
    // until the worker has finished reading, composing and posting the frame.
    pthread_mutex_lock(&d->workMutex);
    d->workDisplay=display;d->workList=list;d->workPending=true;pthread_cond_signal(&d->workCondition);
    while(d->workPending)pthread_cond_wait(&d->workCondition,&d->workMutex);
    const int result=d->workResult;d->workList=nullptr;
    pthread_mutex_unlock(&d->workMutex);return result;
}
int present(hwc_composer_device_1_t* api,size_t count,hwc_display_contents_1_t** lists) {
    if(!lists||count>MaxDisplays)return -EINVAL;
    Device* d=device(api);std::lock_guard<std::mutex> hold(d->render);int result=0;
    for(size_t display=0;display<count;++display) {
        auto* list=lists[display];if(!list)continue;
        if(list->numHwLayers>MaxLayers)return -EINVAL;
        Fences cleanup(list);
        // HWC1 can still pass its primary list during a commit after blank().
        // Release the unused input fences, but never update a powered-off EPDC.
        // blank() and present() share render, so the state covers the whole post.
        if(display==HWC_DISPLAY_PRIMARY&&!d->displayOn) {
            ++d->skippedOffFrames;d->canvasCurrent=false;
            continue;
        }
        const int rc=display==HWC_DISPLAY_PRIMARY||display>=HWC_DISPLAY_VIRTUAL?submit(d,display,list):-ENODEV;
        if(rc) {++d->failures;if(!result)result=rc;if(d->failures<=8)ALOGE("present failed display=%zu error=%d",display,rc);}
    }
    return result;
}
void* vsyncLoop(void* pointer) {
    auto* d=static_cast<Device*>(pointer);pthread_mutex_lock(&d->eventMutex);
    while(!d->stopping) {
        while(!d->stopping&&(!d->vsyncEnabled||!d->displayOn||!d->callbacks))
            pthread_cond_wait(&d->eventCondition,&d->eventMutex);
        if(d->stopping)break;
        const int64_t deadline=now()+d->period;timespec until{time_t(deadline/1000000000),long(deadline%1000000000)};
        const int rc=pthread_cond_timedwait(&d->eventCondition,&d->eventMutex,&until);
        if(rc!=ETIMEDOUT||!d->vsyncEnabled||!d->displayOn||d->stopping)continue;
        const hwc_procs_t* callbacks=d->callbacks;
        pthread_mutex_unlock(&d->eventMutex);
        // Android scheduling pulse, not EPDC scanout or waveform completion.
        if(callbacks&&callbacks->vsync)callbacks->vsync(callbacks,HWC_DISPLAY_PRIMARY,now());
        pthread_mutex_lock(&d->eventMutex);
    }
    pthread_mutex_unlock(&d->eventMutex);return nullptr;
}
int eventControl(hwc_composer_device_1_t* api,int display,int event,int enabled) {
    if(display!=HWC_DISPLAY_PRIMARY||event!=HWC_EVENT_VSYNC||(enabled!=0&&enabled!=1))return -EINVAL;
    Device* d=device(api);pthread_mutex_lock(&d->eventMutex);d->vsyncEnabled=enabled;
    pthread_cond_signal(&d->eventCondition);pthread_mutex_unlock(&d->eventMutex);return 0;
}
void registerProcs(hwc_composer_device_1_t* api,const hwc_procs_t* callbacks) {
    Device* d=device(api);pthread_mutex_lock(&d->eventMutex);d->callbacks=callbacks;
    pthread_cond_signal(&d->eventCondition);pthread_mutex_unlock(&d->eventMutex);
}
int power(Device* d,bool on) {
    std::lock_guard<std::mutex> hold(d->render);
    // The sleep picture, or else the grey levels of pixels still in their first
    // pass, before the panel loses power: an e-ink panel keeps what it shows,
    // so the screen must not keep the page. After a picture the panel needs a
    // complete frame; SurfaceFlinger repaints everything when it turns on.
    if(!on&&d->panel)d->panel->sleep(d->framebuffer);
    if(ioctl(d->blankFd,FBIOBLANK,on?FB_BLANK_UNBLANK:FB_BLANK_POWERDOWN)<0)return -errno;
    pthread_mutex_lock(&d->eventMutex);d->displayOn=on;pthread_cond_signal(&d->eventCondition);
    pthread_mutex_unlock(&d->eventMutex);return 0;
}
int blank(hwc_composer_device_1_t* api,int display,int value) {
    if(display!=HWC_DISPLAY_PRIMARY)return -EINVAL;
    return power(device(api),!value);
}
int setPowerMode(hwc_composer_device_1_t* api,int display,int mode) {
    if(display!=HWC_DISPLAY_PRIMARY)return -EINVAL;
    // The panel keeps its picture without power; doze is the same as on.
    return power(device(api),mode!=HWC_POWER_MODE_OFF);
}
int getActiveConfig(hwc_composer_device_1_t*,int display) {return display==HWC_DISPLAY_PRIMARY?0:-EINVAL;}
int setActiveConfig(hwc_composer_device_1_t*,int display,int index) {return display==HWC_DISPLAY_PRIMARY&&index==0?0:-EINVAL;}
int query(hwc_composer_device_1_t* api,int what,int* value) {
    if(!value)return -EINVAL;
    switch(what) {
        case HWC_BACKGROUND_LAYER_SUPPORTED:*value=0;return 0;
        case HWC_VSYNC_PERIOD:*value=int(device(api)->period);return 0;
        case HWC_DISPLAY_TYPES_SUPPORTED:*value=HWC_DISPLAY_PRIMARY_BIT;return 0;
        default:return -EINVAL;
    }
}
int configs(hwc_composer_device_1_t*,int display,uint32_t* values,size_t* count) {
    if(display!=HWC_DISPLAY_PRIMARY||!count)return -EINVAL;
    if(*count&&values)values[0]=0;else if(*count)return -EINVAL;*count=1;return 0;
}
int attributes(hwc_composer_device_1_t* api,int display,uint32_t config,const uint32_t* keys,int32_t* values) {
    if(display!=HWC_DISPLAY_PRIMARY||config||!keys||!values)return -EINVAL;
    Device* d=device(api);
    for(size_t i=0;i<16;++i) {
        switch(keys[i]) {
            case HWC_DISPLAY_NO_ATTRIBUTE:return 0;
            case HWC_DISPLAY_VSYNC_PERIOD:values[i]=int(d->period);break;
            case HWC_DISPLAY_COLOR_TRANSFORM:values[i]=0;break;
            case HWC_DISPLAY_WIDTH:values[i]=d->framebuffer->width;break;
            case HWC_DISPLAY_HEIGHT:values[i]=d->framebuffer->height;break;
            case HWC_DISPLAY_DPI_X:values[i]=int(d->framebuffer->xdpi*1000);break;
            case HWC_DISPLAY_DPI_Y:values[i]=int(d->framebuffer->ydpi*1000);break;
            default:return -EINVAL;
        }
    }
    return -EINVAL;
}
void dump(hwc_composer_device_1_t* api,char* text,int size) {
    if(!text||size<=0)return;
    Device* d=device(api);std::lock_guard<std::mutex> hold(d->render);
    const int used=snprintf(text,size,"PW3 software composer: cpu_frames=%llu gles_frames=%llu empty_frames=%llu failures=%llu compose_us=%lld post_us=%lld skipped_off_frames=%llu"
             " partial_frames=%llu idle_frames=%llu compose_us_total=%llu post_us_total=%llu compose_us_max=%lld post_us_max=%lld dim_frames=%llu"
             " composed_pixels_total=%llu vsync_hz=%d damage=%d verified_frames=%llu verify_failures=%llu"
             " unchanged_areas=%llu unchanged_pixels=%llu unchanged_frames=%llu skip_unchanged=%d\n",
             (unsigned long long)d->cpuFrames,(unsigned long long)d->fallbackFrames,
             (unsigned long long)d->emptyFrames,(unsigned long long)d->failures,
             (long long)d->lastComposeUs,(long long)d->lastPostUs,(unsigned long long)d->skippedOffFrames,
             (unsigned long long)d->partialFrames,(unsigned long long)d->idleFrames,(unsigned long long)d->composeUsTotal,
             (unsigned long long)d->postUsTotal,(long long)d->maxComposeUs,(long long)d->maxPostUs,(unsigned long long)d->dimFrames,
             (unsigned long long)d->composedPixels,int(1000000000/d->period),d->useDamage?1:0,
             (unsigned long long)d->verifiedFrames,(unsigned long long)d->verifyFailures,
             (unsigned long long)d->unchangedAreas,(unsigned long long)d->unchangedPixels,
             (unsigned long long)d->unchangedFrames,d->skipUnchanged?1:0);
    if(d->panel&&used>0&&used<size)d->panel->describe(d->framebuffer,text+used,size-used);
}
int closeDevice(hw_device_t* api) {delete reinterpret_cast<Device*>(api);return 0;}
int openDevice(const hw_module_t* module,const char* name,hw_device_t** output) {
    if(!module||!name||!output||strcmp(name,HWC_HARDWARE_COMPOSER))return -EINVAL;
    auto* d=new(std::nothrow) Device;if(!d)return -ENOMEM;
    const hw_module_t* gm=nullptr;int rc=hw_get_module(GRALLOC_HARDWARE_MODULE_ID,&gm);
    if(!rc){d->gralloc=reinterpret_cast<const gralloc_module_t*>(gm);rc=framebuffer_open(gm,&d->framebuffer);}
    if(!rc) {
        d->panel=pw3PanelOps(d->framebuffer);
        if(d->panel) {
            d->canvas.luma=d->panel->canvas(d->framebuffer,&d->canvas.width,&d->canvas.height);
            if(!d->canvas.luma||d->canvas.width!=int(d->framebuffer->width)||d->canvas.height!=int(d->framebuffer->height))rc=-EINVAL;
        }
    }
    if(!rc){d->blankFd=::open("/dev/graphics/fb0",O_RDWR|O_CLOEXEC);if(d->blankFd<0)rc=-errno;}
    // Android's frame clock. The panel needs 134 ms or more for one update,
    // so this only bounds how often applications draw and how long a touch
    // waits for the next frame. debug.pw3.vsync_hz: 5..60.
    const int hz=property_get_int32("debug.pw3.vsync_hz",30);
    d->period=1000000000/(hz<5?5:hz>60?60:hz);
    d->useDamage=property_get_bool("debug.pw3.hwc.damage",true);
    pthread_condattr_t options;
    if(!rc) {
        rc=-pthread_condattr_init(&options);
        if(!rc) {
            rc=-pthread_condattr_setclock(&options,CLOCK_MONOTONIC);
            if(!rc){rc=-pthread_cond_init(&d->eventCondition,&options);d->conditionReady=!rc;}
            pthread_condattr_destroy(&options);
        }
    }
    if(!rc){rc=-pthread_create(&d->eventThread,nullptr,vsyncLoop,d);d->threadStarted=!rc;}
    if(!rc) {
        rc=-pthread_create(&d->workThread,nullptr,compositionLoop,d);d->workStarted=!rc;
        if(!rc) {
            pthread_mutex_lock(&d->workMutex);
            while(!d->workerReady)pthread_cond_wait(&d->workCondition,&d->workMutex);
            if(d->workerReady<0)rc=d->workerReady;
            pthread_mutex_unlock(&d->workMutex);
        }
    }
    if(rc){delete d;return rc;}
    d->diagnostic=property_get_bool("ro.debuggable",false);
    d->api.common.tag=HARDWARE_DEVICE_TAG;d->api.common.version=HWC_DEVICE_API_VERSION_1_5;
    d->api.common.module=const_cast<hw_module_t*>(module);d->api.common.close=closeDevice;
    d->api.prepare=prepare;d->api.set=present;d->api.eventControl=eventControl;d->api.blank=blank;
    d->api.query=query;d->api.registerProcs=registerProcs;d->api.dump=dump;
    d->api.getDisplayConfigs=configs;d->api.getDisplayAttributes=attributes;
    d->api.setPowerMode=setPowerMode;d->api.getActiveConfig=getActiveConfig;d->api.setActiveConfig=setActiveConfig;
    ALOGI("CPU luma composer ready %ux%u panel=%d vsync_hz=%d damage=%d; unsupported layers use GLES",d->framebuffer->width,
          d->framebuffer->height,d->panel?1:0,int(1000000000/d->period),d->useDamage?1:0);
    *output=&d->api.common;return 0;
}
hw_module_methods_t methods={openDevice};
}
extern "C" {
hwc_module_t HAL_MODULE_INFO_SYM={
    {HARDWARE_MODULE_TAG,HWC_MODULE_API_VERSION_0_1,HARDWARE_HAL_API_VERSION,
     HWC_HARDWARE_MODULE_ID,"PW3 CPU luma composer with GLES fallback","Readom",&methods,nullptr,{0}}
};
}
