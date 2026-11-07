// Renderer.m - uploads the game's frame and presents it with the configured
// scaling mode, aspect ratio and output filter.
#import "Renderer.h"
#import "GameView.h"
#import "InputManager.h"
#include "rt.h"

typedef struct {
    float src_w, src_h, out_w, out_h;
    int32_t filter;
    float scanline, sharpness, pad;
} PresentParams;

@implementation Renderer {
    __weak MTKView *_view;
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    id<MTLTexture> _frameTex;
    uint64_t _uploaded;
    uint32_t _texW, _texH;
}

- (instancetype)initWithView:(MTKView *)view {
    if ((self = [super init])) {
        _view = view;
        _device = view.device;
        _queue = [_device newCommandQueue];
        [self buildPipeline];
    }
    return self;
}

- (void)buildPipeline {
    NSError *err = nil;
    id<MTLLibrary> lib = [_device newDefaultLibrary];
    if (!lib) {
        // compile from source shipped in the bundle (works with just the Command Line Tools)
        NSString *path = [[NSBundle mainBundle] pathForResource:@"Shaders" ofType:@"metal"];
        NSString *src = path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&err] : nil;
        if (src) lib = [_device newLibraryWithSource:src options:nil error:&err];
    }
    if (!lib) { NSLog(@"shader library unavailable: %@", err); return; }
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = [lib newFunctionWithName:@"present_vs"];
    d.fragmentFunction = [lib newFunctionWithName:@"present_fs"];
    d.colorAttachments[0].pixelFormat = _view.colorPixelFormat;
    _pipeline = [_device newRenderPipelineStateWithDescriptor:d error:&err];
    if (!_pipeline) NSLog(@"pipeline error: %@", err);
}

- (void)settingsChanged { /* parameters are read every frame */ }

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {}

- (void)uploadFrame:(const RtFrame *)fr {
    if (!_frameTex || _texW != fr->width || _texH != fr->height) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:fr->width height:fr->height mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        _frameTex = [_device newTextureWithDescriptor:td];
        _texW = fr->width; _texH = fr->height;
    }
    [_frameTex replaceRegion:MTLRegionMake2D(0, 0, fr->width, fr->height) mipmapLevel:0
                   withBytes:fr->rgba bytesPerRow:fr->width * 4];
    _uploaded = fr->frame;
}

/* destination rectangle in drawable pixels */
static MTLViewport viewport_for(CGSize out, uint32_t sw, uint32_t sh, bool wide) {
    double W = out.width, H = out.height;
    double aspect;
    switch (rt_settings.aspect) {
        case RT_ASPECT_4_3: aspect = 4.0 / 3.0; break;
        case RT_ASPECT_16_9: aspect = 16.0 / 9.0; break;
        default: aspect = wide ? 16.0 / 9.0 : 4.0 / 3.0; break;
    }
    double w = W, h = H;
    switch (rt_settings.scale_mode) {
        case RT_SCALE_STRETCH: break;
        case RT_SCALE_INTEGER: {
            int k = (int)floor(H / sh);
            while (k > 1 && k * sh * aspect > W) k--;
            if (k < 1) k = 1;
            h = k * (double)sh;
            w = h * aspect;
            if (w > W) { w = W; h = W / aspect; }
            break;
        }
        default:
            if (W / H > aspect) { h = H; w = H * aspect; } else { w = W; h = W / aspect; }
            break;
    }
    MTLViewport vp = { floor((W - w) / 2), floor((H - h) / 2), floor(w), floor(h), 0, 1 };
    return vp;
}

- (void)drawInMTKView:(MTKView *)view {
    [[InputManager shared] poll];
    RtFrame fr;
    bool have = rt_video_frame(&fr);
    if (have && fr.frame != _uploaded) [self uploadFrame:&fr];

    MTLRenderPassDescriptor *rp = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (!rp || !drawable) return;
    id<MTLCommandBuffer> cb = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (_frameTex && _pipeline) {
        MTLViewport vp = viewport_for(view.drawableSize, _texW, _texH, have && fr.widescreen);
        [enc setViewport:vp];
        PresentParams p = { (float)_texW, (float)_texH, (float)vp.width, (float)vp.height,
                            (int32_t)rt_settings.output_filter, rt_settings.crt_scanline, rt_settings.sharpness, 0 };
        [enc setRenderPipelineState:_pipeline];
        [enc setFragmentTexture:_frameTex atIndex:0];
        [enc setFragmentBytes:&p length:sizeof p atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [enc endEncoding];
    [cb presentDrawable:drawable];
    [cb commit];
    if ([view isKindOfClass:[GameView class]]) [(GameView *)view didPresentGameFrame:_uploaded];
}

@end
