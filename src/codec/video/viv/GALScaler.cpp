// COPYRIGHT Deepin Inc.

#include "GALScaler.h"
#include "ImageConverter_p.h"
#include <galUtil.h>
#include "utils/Logger.h"

#define GC_ENSURE(x, ...) \
    do { \
        gceSTATUS ret = x; \
        if (ret < 0) { \
            qWarning("VIV error@%d. " #x "(%d): %s", __LINE__, ret, gcoOS_DebugStatus2Name(ret)); \
            return __VA_ARGS__; \
        } \
    } while(0);
#define GC_WARN(x) \
do { \
  gceSTATUS ret = x; \
  if(ret < 0) \
    qWarning("VIV error@%d. " #x "(%d): %s", __LINE__, ret, gcoOS_DebugStatus2Name(ret)); \
} while(0);

extern "C" {
void dma_copy_in_vmem(unsigned int dst, unsigned int src, int len);
void dma_copy_from_vmem(unsigned char* dst, unsigned int src, int len);
}
namespace QtAV {
struct GALSurface {
    gcoSURF surf;
    gceSURF_FORMAT format;
    gctUINT width, height;
    gctINT stride;
    gctUINT32 phyAddr[3];
    gctPOINTER lgcAddr[3];

    GALSurface() : surf(gcvNULL), format(gcvSURF_UNKNOWN), width(0), height(0), stride(0)
    {
        memset(phyAddr, 0, sizeof(phyAddr));
        memset(lgcAddr, 0, sizeof(lgcAddr));
    }
    ~GALSurface() { destroy();}
    void destroy() {
        unlock();
        if (surf != gcvNULL) {
            GC_ENSURE(gcoSURF_Destroy(surf));
            surf = gcvNULL;
        }
    }
    bool lock() {
        if (surf)
            GC_ENSURE(gcoSURF_Lock(surf, phyAddr, lgcAddr), false);
        return true;
    }
    bool unlock() {
        if (surf && lgcAddr[0] != gcvNULL) { //TODO: unlock for every plane?
            GC_ENSURE(gcoSURF_Unlock(surf, lgcAddr[0]), false);
            memset(lgcAddr, 0, sizeof(lgcAddr));
        }
        return true;
    }
};
class AutoLock {
    GALSurface *surf;
public:
    AutoLock(GALSurface *s) : surf(s) {
        if (surf)
            surf->lock();
    }
    ~AutoLock() {
        if (surf) {
            surf->unlock();
        }
    }
};

class GALScalerPrivate Q_DECL_FINAL: public ImageConverterPrivate
{
public:
    GALScalerPrivate() : ImageConverterPrivate()
      , gc_os(gcvNULL)
      , gc_hal(gcvNULL)
      , gc_2d(gcvNULL)
      , contiguous_size(0)
      , contiguous(gcvNULL)
      , contiguous_phys(gcvNULL)
    {}
    ~GALScalerPrivate() {
        close();
    }
    bool open(int w, int h, gceSURF_FORMAT fmt); // must reset surfaces if false
    bool close();
    bool createSourceSurface(int w, int h, gceSURF_FORMAT fmt);
    gcoOS gc_os;
    gcoHAL gc_hal;
    gco2D gc_2d;
    gctSIZE_T contiguous_size;
    gctPOINTER contiguous;
    gctPHYS_ADDR contiguous_phys;

    GALSurface surf_in;
    GALSurface surf_out;
};

typedef struct {
    gceSURF_FORMAT gc;
    VideoFormat::PixelFormat pixfmt;
} gc_fmt_entry;
// TODO: more formats and confirm
static const gc_fmt_entry gc_fmts[] = {
    { gcvSURF_A8R8G8B8, VideoFormat::Format_RGB32},
    { gcvSURF_A8R8G8B8, VideoFormat::Format_ARGB32},
    { gcvSURF_A8R8G8B8, VideoFormat::Format_BGRA32},
    { gcvSURF_R5G6B5, VideoFormat::Format_RGB565 },
    { gcvSURF_I420, VideoFormat::Format_YUV420P }
};
static gceSURF_FORMAT pixelFormatToGAL(VideoFormat::PixelFormat pixfmt)
{
    for (const gc_fmt_entry* e = gc_fmts; e < gc_fmts + sizeof(gc_fmts)/sizeof(gc_fmts[0]); ++e)
        if (e->pixfmt == pixfmt)
            return e->gc;
    return gcvSURF_UNKNOWN;
}

GALScaler::GALScaler()
    : ImageConverter(*new GALScalerPrivate())
{}

bool GALScaler::check() const
{
    if (!ImageConverter::check())
        return false;
    DPTR_D(const GALScaler);
    return VideoFormat(VideoFormat::pixelFormatFromFFmpeg(d.fmt_out)).isRGB();
}

bool GALScaler::convert(const quint8 * const src[], const int srcStride[])
{
    DPTR_D(GALScaler);
    const gceSURF_FORMAT srcFmt = pixelFormatToGAL(VideoFormat::pixelFormatFromFFmpeg(d.fmt_in));
    if (!d.surf_in.surf
            || d.surf_in.width != (gctUINT)FFALIGN(d.w_in,16)
            || d.surf_in.height != (gctUINT)FFALIGN(d.h_in,16)
            || d.surf_in.format != srcFmt) {
        d.surf_in.destroy();
        if (!d.createSourceSurface(d.w_in, d.h_in, srcFmt)) {
            return false;
        }
    }
    AutoLock lock(&d.surf_in);
    Q_UNUSED(lock);
    const VideoFormat fmt(d.fmt_in);
    // d.w_in*d.h_in, 1/4, 1/4
    // TODO: use gcoSURF api to get stride, height etc
    for (int i = 0; i < fmt.planeCount(); ++i) {
        // src[2] is 0x0!
        //qDebug("dma_copy_in_vmem %d: %p=>%p len:%d", i, src[i], d.surf_in.phyAddr[i], srcStride[i]*fmt.height(d.h_in, i));
        dma_copy_in_vmem(d.surf_in.phyAddr[i], (gctUINT32)(quintptr)src[i], srcStride[i]*fmt.height(d.h_in, i));
    }
    // TODO: setup d.gc_2d only if parameters changed
    gcsRECT dstRect = {0, 0, (gctINT32)d.surf_out.width, (gctINT32)d.surf_out.height};
    GC_ENSURE(gco2D_SetClipping(d.gc_2d, &dstRect), false);
    GC_ENSURE(gcoSURF_SetDither(d.surf_out.surf, gcvTRUE), false);
    gctUINT8 horKernel = 1, verKernel = 1;
    GC_ENSURE(gco2D_SetKernelSize(d.gc_2d, horKernel, verKernel), false); //TODO: check ok not < 0?
    GC_ENSURE(gco2D_EnableDither(d.gc_2d, gcvTRUE), false);
    gcsRECT srcRect = { 0, 0, d.w_in, d.h_in};
    GC_ENSURE(gcoSURF_FilterBlit(d.surf_in.surf, d.surf_out.surf, &srcRect, &dstRect, &dstRect), false)
    GC_ENSURE(gco2D_EnableDither(d.gc_2d, gcvFALSE), false)
    GC_ENSURE(gco2D_Flush(d.gc_2d), false);
    GC_ENSURE(gcoHAL_Commit(d.gc_hal, gcvTRUE), false);
    return true;
}

bool GALScaler::convert(const quint8 *const src[], const int srcStride[], quint8 *const dst[], const int dstStride[])
{
    DPTR_D(GALScaler);
    AutoLock lock(&d.surf_out);
    Q_UNUSED(lock);
    // lock dstSurf, copy from dstSurf, unlock dstSurf
    if (!convert(src, srcStride))
        return false;
    const VideoFormat fmt(d.fmt_out);
    for (int p = 0; p < fmt.planeCount(); ++p) {
        // dma copy. check img->stride
        if (d.pitchs.at(p) == dstStride[p]) {
            // qMin(scaler->outHeight(), img->height)
            dma_copy_from_vmem(dst[p], (unsigned int)(quintptr)d.bits.at(p), dstStride[p]*fmt.height(d.h_out, p));
        } else {
            qWarning("different stride @plane%d. vmem: %d, host: %d", p, d.pitchs.at(p), dstStride[p]);
            for (int i = fmt.height(d.h_out, p) - 1; i >= 0; --i)
                dma_copy_from_vmem(dst[p] + i*dstStride[p], (unsigned int)(quintptr)d.bits.at(p) + i*d.pitchs.at(p), dstStride[p]);
        }
    }
    return true;
}

bool GALScaler::prepareData() // only for output
{
    DPTR_D(GALScaler);
    if (!check())
        return false;
    const int nb_planes = qMax(av_pix_fmt_count_planes(d.fmt_out), 0);
    qDebug("prepare GAL resource.%dx%d=>%dx%d nb_planes: %d. gcfmt: %d", d.w_in, d.h_in, d.w_out, d.h_out, nb_planes, pixelFormatToGAL(VideoFormat::pixelFormatFromFFmpeg(d.fmt_out)));
    d.bits.resize(nb_planes);
    d.pitchs.resize(nb_planes);
    d.close();
    //d.surf_out.destroy(); // destroy out surface is enough?
    const VideoFormat fmt(d.fmt_out);
    if (!d.open(d.w_out, d.h_out, pixelFormatToGAL(fmt.pixelFormat())))
        return false;
    if (d.surf_out.lock())
        d.surf_out.unlock();
    // if unlock here, we must lock again to when copying from gal
    qDebug("lock surf_out.surf: %p, phy:%p, lgc:%p", d.surf_out.surf, (quintptr)d.surf_out.phyAddr[0], d.surf_out.lgcAddr[0]);
    for (int i = 0; i < nb_planes; ++i) {
        d.bits[i] = (quint8*)(quintptr)d.surf_out.phyAddr[i];
        d.pitchs[i] = fmt.width(d.surf_out.stride, i);
    }
    qDebug() << "surf_out bits/pitch:" << d.bits << d.pitchs;
    return true;
}

bool GALScalerPrivate::open(int w, int h, gceSURF_FORMAT fmt)
{
    /* Construct the gcoOS object. */
    GC_ENSURE(gcoOS_Construct(gcvNULL, &gc_os), false);
    qDebug("gcoOS_Construct, gc_os:%p", gc_os);
    /* Construct the gcoHAL object. */
    GC_ENSURE(gcoHAL_Construct(gcvNULL, gc_os, &gc_hal), false);
    // gcvFEATURE_YUV420_TILER for tiled map?
    if (!gcoHAL_IsFeatureAvailable(gc_hal, gcvFEATURE_YUV420_SCALER)) {
        qWarning("VIV: I420 scaler is not supported.");
        return false;
    }
    GC_ENSURE(gcoHAL_QueryVideoMemory(gc_hal, NULL, NULL, NULL, NULL, &contiguous_phys, &contiguous_size), false);
    if (contiguous_size <= 0) {
        qWarning("invalid contiguous_size: %lu", contiguous_size);
        return false;
    }
    /* Map the contiguous memory. */
    GC_ENSURE(gcoHAL_MapMemory(gc_hal, contiguous_phys, contiguous_size, &contiguous), false);
    GC_ENSURE(gcoHAL_Get2DEngine(gc_hal, &gc_2d), false);
    GC_ENSURE(gcoSURF_Construct(gc_hal, w, h,
                    1, //TODO: depth. why 1?
                    gcvSURF_BITMAP, fmt, gcvPOOL_DEFAULT, &surf_out.surf), false);
    GC_ENSURE(gcoSURF_GetAlignedSize(surf_out.surf, &surf_out.width, &surf_out.height, &surf_out.stride), false);
    qDebug("aligned surf_out: %dx%d, stride:%d", surf_out.width, surf_out.height, surf_out.stride);
    return true;
}

bool GALScalerPrivate::close()
{
    // destroy source surface and hal/os/contiguous?
    surf_in.destroy();
    surf_out.destroy();
    if (contiguous != gcvNULL) {
        qDebug("unmap contiguous:%p, phy: %p, size: %lu", contiguous, contiguous_phys, contiguous_size);
        /* Unmap the contiguous memory. */
        GC_WARN(gcoHAL_UnmapMemory(gc_hal, contiguous_phys, contiguous_size, contiguous));
    }
    if (gc_hal != gcvNULL) {
        GC_WARN(gcoHAL_Commit(gc_hal, gcvTRUE));
        GC_WARN(gcoHAL_Destroy(gc_hal));
        gc_hal = gcvNULL;
    }
    if (gc_os != gcvNULL) {
        GC_WARN(gcoOS_Destroy(gc_os));
        gc_os = gcvNULL;
    }
    return true;
}

bool GALScalerPrivate::createSourceSurface(int w, int h, gceSURF_FORMAT fmt)
{
    gcoSURF srcsurf = NULL;
    GC_ENSURE(gcoSURF_Construct(gc_hal, w, h, 1, gcvSURF_BITMAP, fmt, gcvPOOL_DEFAULT, &srcsurf), false);
    gctUINT aw, ah;
    gctINT astride;
    // alignment is 16. already aligned because the source if from frame buffer
    gcmVERIFY_OK(gcoSURF_GetAlignedSize(srcsurf, &aw, &ah, &astride));
    qDebug("srcsurf:%p, %dx%d=>%dx%d, alignedStride:%d", srcsurf, w, h, aw, ah, astride);
    if ((gctUINT)w != aw || (gctUINT)h != ah) { // We can ignore it!
        qWarning("gcoSURF width and height is not aligned!");
        GC_ENSURE(gcoSURF_Destroy(srcsurf), false);
        return false;
    }
    surf_in.surf = srcsurf;
    GC_ENSURE(gcoSURF_GetAlignedSize(surf_in.surf, gcvNULL, gcvNULL, &surf_in.stride), false);
    GC_ENSURE(gcoSURF_GetSize(surf_in.surf, &surf_in.width, &surf_in.height, gcvNULL), false);
    GC_ENSURE(gcoSURF_GetFormat(surf_in.surf, gcvNULL, &surf_in.format), false);
    qDebug("surf_in: %dx%d, stride: %d, fomrat:%d", surf_in.width, surf_in.height, surf_in.stride, surf_in.format);
    return true;
}
} //namespace QtAV