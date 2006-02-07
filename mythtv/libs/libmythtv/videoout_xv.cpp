/* Based on xqcam.c by Paul Chinn <loomer@svpal.org> */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cerrno>

#include <malloc.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/keysym.h>

#include <iostream>

#include "yuv2rgb.h"
#include "osd.h"
#include "osdsurface.h"
#include "osdxvmc.h"
#include "mythcontext.h"
#include "filtermanager.h"
#include "videoout_xv.h"
#include "XvMCSurfaceTypes.h"
#include "util-x11.h"
#include "config.h"
#include "exitcodes.h"

#define LOC QString("VideoOutputXv: ")
#define LOC_ERR QString("VideoOutputXv Error: ")

extern "C" {
#define XMD_H 1
#include <X11/extensions/xf86vmode.h>
#include <X11/extensions/Xinerama.h>
    extern int      XShmQueryExtension(Display*);
    extern int      XShmGetEventBase(Display*);
    extern XvImage  *XvShmCreateImage(Display*, XvPortID, int, char*,
                                      int, int, XShmSegmentInfo*);
}

static QString xvflags2str(int flags);
static void clear_xv_buffers(VideoBuffers&, int w, int h, int xv_chroma);

#ifndef HAVE_ROUND
#define round(x) ((int) ((x) + 0.5))
#endif

class port_info { public: Display *disp; int port; };
static QMap<int,port_info> open_xv_ports;

static void close_all_xv_ports_signal_handler(int sig)
{
    cerr<<"Signal: "<<sys_siglist[sig]<<endl;
    QMap<int,port_info>::iterator it;
    for (it = open_xv_ports.begin(); it != open_xv_ports.end(); ++it)
    {
        cerr<<"Ungrabbing XVideo port: "<<(*it).port<<endl;
        XvUngrabPort((*it).disp, (*it).port, CurrentTime);
    }
    exit(GENERIC_EXIT_NOT_OK);
}
static void add_open_xv_port(Display *disp, int port)
{
    if (port >= 0)
    {
        open_xv_ports[port].disp = disp;
        open_xv_ports[port].port = port;
        // TODO enable more catches after 0.19 is out -- dtk
        signal(SIGINT,  close_all_xv_ports_signal_handler);
    }
}
static void del_open_xv_port(int port)
{
    if (port >= 0)
    {
        open_xv_ports.remove(port);

        if (!open_xv_ports.count())
        {
            // TODO enable more catches 0.19 is out -- dtk
            signal(SIGINT, SIG_DFL);
        }
    }
}
static bool has_open_xv_port(int port)
{
    return open_xv_ports.find(port) != open_xv_ports.end();
}
static uint cnt_open_xv_port(void)
{
    return open_xv_ports.count();
}

//#define DEBUG_PAUSE /* enable to debug XvMC pause frame */

#ifdef USING_XVMC
    static inline xvmc_render_state_t *GetRender(VideoFrame *frame);

#   if defined(USING_XVMCW) || defined(USING_XVMC_VLD)
        extern "C" Status XvMCPutSlice2(Display*,XvMCContext*,char*,int,int);
#   else
        Status XvMCPutSlice2(Display*, XvMCContext*, char*, int, int)
            { return XvMCBadSurface; }
#   endif
    static inline QString ErrorStringXvMC(int);
#endif // USING_XVMC

// See http://www.fourcc.org/yuv.php for more info on formats
#define GUID_I420_PLANAR 0x30323449
#define GUID_IYUV_PLANAR 0x56555949 /**< bit equivalent to I420 */
#define GUID_YV12_PLANAR 0x32315659

static void SetFromEnv(bool &useXvVLD, bool &useXvIDCT, bool &useXvMC,
                       bool &useXV, bool &useShm);
static void SetFromHW(Display *d, bool &useXvMC, bool &useXV, bool& useShm);

class XvMCBufferSettings
{
  public:
    XvMCBufferSettings() :
        num_xvmc_surf(1),
        needed_for_display(1),
        min_num_xvmc_surfaces(8),
        max_num_xvmc_surfaces(16),
        num_xvmc_surfaces(min_num_xvmc_surfaces),
        aggressive(false) {}

    void SetOSDNum(uint val)
    {
        num_xvmc_surf = val;
    }

    void SetNumSurf(uint val)
    {
        num_xvmc_surfaces = min(max(val, min_num_xvmc_surfaces),
                                max_num_xvmc_surfaces);
    }

    /// Returns number of XvMC OSD surfaces to allocate
    uint GetOSDNum(void)    const { return num_xvmc_surf; }

    /// Returns number of frames we want decoded before we
    /// try to display a frame.
    uint GetNeededBeforeDisplay(void)
        const { return needed_for_display; }

    /// Returns minumum number of XvMC surfaces we need
    uint GetMinSurf(void) const { return min_num_xvmc_surfaces; }

    /// Returns maximum number of XvMC surfaces should try to get
    uint GetMaxSurf(void) const { return max_num_xvmc_surfaces; }

    /// Returns number of XvMC surfaces we actually allocate
    uint GetNumSurf(void) const { return num_xvmc_surfaces; }

    /// Returns number of frames we want to try to prebuffer
    uint GetPreBufferGoal(void) const
    {
        uint reserved = GetFrameReserve() + XVMC_PRE_NUM +
            XVMC_POST_NUM + XVMC_SHOW_NUM;
        return num_xvmc_surfaces - reserved;
    }

    /// Returns number of frames reserved for the OSD blending process
    /// and for video display. This is the HARD reserve.
    uint GetFrameReserve(void) const
        { return num_xvmc_surf + XVMC_SHOW_NUM; }

    /// Returns true if we should be aggressive in freeing buffers
    bool IsAggressive(void)  const { return aggressive; }

  private:
    /// Number of XvMC OSD surface to allocate
    uint num_xvmc_surf;
    /// Frames needed before we try to display a frame, a larger
    /// number here ensures that we don't lose A/V Sync when a 
    /// frame takes longer than one frame interval to decode.
    uint needed_for_display;
    /// Minumum number of XvMC surfaces to get
    uint min_num_xvmc_surfaces;
    /// Maximum number of XvMC surfaces to get
    uint max_num_xvmc_surfaces;
    /// Number of XvMC surfaces we got
    uint num_xvmc_surfaces;
    /// Use aggressive buffer management
    bool aggressive;

    /// Allow for one I/P frame before us
    static const uint XVMC_PRE_NUM  = 1;
    /// Allow for one I/P frame after us
    static const uint XVMC_POST_NUM = 1;
    /// Allow for one B frame to be displayed
    static const uint XVMC_SHOW_NUM = 1;
};

class ChromaKeyOSD
{
  public:
    ChromaKeyOSD(VideoOutputXv *vo) :
        videoOutput(vo), current(-1), revision(-1)
    {
        bzero(vf,        2 * sizeof(VideoFrame));
        bzero(img,       2 * sizeof(XImage*));
        bzero(shm_infos, 2 * sizeof(XShmSegmentInfo));
    }

    bool ProcessOSD(OSD *osd);
    void AllocImage(int i);
    void FreeImage(int i);
    void Clear(int i);
    void Reset(void) { current = -1; revision = -1; }

    XImage *GetImage() { return (current < 0) ? NULL : img[current]; }

  private:
    void Reinit(int i);

    VideoOutputXv   *videoOutput;
    int              current;
    int              revision;
    VideoFrame       vf[2];
    XImage          *img[2];
    XShmSegmentInfo  shm_infos[2];
};

/** \class  VideoOutputXv
 * Supports common video output methods used with %X11 Servers.
 *
 * This class suppurts XVideo with VLD acceleration (XvMC-VLD), XVideo with
 * inverse discrete cosine transform (XvMC-IDCT) acceleration, XVideo with 
 * motion vector (XvMC) acceleration, and normal XVideo with color transform
 * and scaling acceleration only. When none of these will work, we also try 
 * to use X Shared memory, and if that fails we try standard Xlib output.
 *
 * \see VideoOutput, VideoBuffers
 *
 */
VideoOutputXv::VideoOutputXv(MythCodecID codec_id)
    : VideoOutput(),
      myth_codec_id(codec_id), video_output_subtype(XVUnknown),
      display_res(NULL), global_lock(true),

      XJ_root(0),  XJ_win(0), XJ_curwin(0), XJ_gc(0), XJ_screen(NULL),
      XJ_disp(NULL), XJ_screen_num(0), XJ_white(0), XJ_black(0), XJ_depth(0),
      XJ_screenx(0), XJ_screeny(0), XJ_screenwidth(0), XJ_screenheight(0),
      XJ_started(false),

      XJ_non_xv_image(0), non_xv_frames_shown(0), non_xv_show_frame(1),
      non_xv_fps(0), non_xv_av_format(PIX_FMT_NB), non_xv_stop_time(0),

#ifdef USING_XVMC
      xvmc_buf_attr(new XvMCBufferSettings()),
      xvmc_chroma(XVMC_CHROMA_FORMAT_420), xvmc_ctx(NULL),
      xvmc_osd_lock(false),
#endif

      xv_port(-1), xv_colorkey(0), xv_draw_colorkey(false), xv_chroma(0),
      xv_color_conv_buf(NULL),

      chroma_osd(NULL)
{
    VERBOSE(VB_PLAYBACK, LOC + "ctor");
    bzero(&av_pause_frame, sizeof(av_pause_frame));

    // If using custom display resolutions, display_res will point
    // to a singleton instance of the DisplayRes class
    if (gContext->GetNumSetting("UseVideoModes", 0))
        display_res = DisplayRes::GetDisplayRes();
}

VideoOutputXv::~VideoOutputXv()
{
    VERBOSE(VB_PLAYBACK, LOC + "dtor");
    if (XJ_started) 
    {
        X11L;
        XSetForeground(XJ_disp, XJ_gc, XJ_black);
        XFillRectangle(XJ_disp, XJ_curwin, XJ_gc,
                       dispx, dispy, dispw, disph);
        X11U;

        m_deinterlacing = false;
    }

    DeleteBuffers(VideoOutputSubType(), true);

    // ungrab port...
    if (xv_port >= 0)
    {
        VERBOSE(VB_PLAYBACK, LOC + "Closing XVideo port " << xv_port);
        X11L;
        XvUngrabPort(XJ_disp, xv_port, CurrentTime);
        del_open_xv_port(xv_port);
        X11U;
        xv_port = -1;
    }

    if (XJ_started) 
    {
        XJ_started = false;

        X11L;
        XFreeGC(XJ_disp, XJ_gc);
        XCloseDisplay(XJ_disp);
        X11U;
    }

    // Switch back to desired resolution for GUI
    if (display_res)
        display_res->SwitchToGUI();
}

// this is documented in videooutbase.cpp
void VideoOutputXv::Zoom(int direction)
{
    QMutexLocker locker(&global_lock);
    VideoOutput::Zoom(direction);
    MoveResize();
}

// this is documented in videooutbase.cpp
void VideoOutputXv::MoveResize(void)
{
    QMutexLocker locker(&global_lock);
    VideoOutput::MoveResize();
    if (chroma_osd)
    {
        chroma_osd->Reset();
        needrepaint = true;
    }
}

// documented in videooutbase.cpp
void VideoOutputXv::InputChanged(int width, int height, float aspect)
{
    VERBOSE(VB_PLAYBACK, LOC + QString("InputChanged(%1,%2,%3)")
            .arg(width).arg(height).arg(aspect));

    QMutexLocker locker(&global_lock);

    bool res_changed = (width != XJ_width) || (height != XJ_height);
    bool asp_changed = aspect != videoAspect;

    VideoOutput::InputChanged(width, height, aspect);

    if (!res_changed)
    {
        if (VideoOutputSubType() == XVideo)
            clear_xv_buffers(vbuffers, XJ_width, XJ_height, xv_chroma);
        if (asp_changed)
            MoveResize();
        return;
    }

    DeleteBuffers(VideoOutputSubType(), false);
    ResizeForVideo((uint) width, (uint) height);
    bool ok = CreateBuffers(VideoOutputSubType());
    MoveResize();

    if (!ok)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "InputChanged(): "
                "Failed to recreate buffers");
        errored = true;
    }
}

// documented in videooutbase.cpp
QRect VideoOutputXv::GetVisibleOSDBounds(
    float &visible_aspect, float &font_scaling) const
{
    if (!chroma_osd)
        return VideoOutput::GetVisibleOSDBounds(visible_aspect, font_scaling);

    float dispPixelAdj = (GetDisplayAspect() * disph) / dispw;
    visible_aspect = 1.3333f/dispPixelAdj;
    font_scaling   = 1.0f;
    return QRect(0,0,dispw,disph);
}

// documented in videooutbase.cpp
QRect VideoOutputXv::GetTotalOSDBounds(void) const
{
    return (chroma_osd) ?
        QRect(0,0,dispw,disph) : QRect(0,0,XJ_width,XJ_height);
}

/**
 * \fn VideoOutputXv::GetRefreshRate(void)
 *
 * This uses the XFree86 xf86vmode extension to query the mode line
 * It then uses the mode line to guess at the refresh rate.
 *
 * \bug This works for all user specified mode lines, but sometimes
 * fails for autogenerated mode lines.
 *
 * \return integer approximation of monitor refresh rate.
 */

int VideoOutputXv::GetRefreshRate(void)
{
    if (!XJ_started)
        return -1;

    XF86VidModeModeLine mode_line;
    int dot_clock;

    int ret = False;
    X11S(ret = XF86VidModeGetModeLine(XJ_disp, XJ_screen_num,
                                      &dot_clock, &mode_line));
    if (!ret)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "GetRefreshRate(): "
                "X11 ModeLine query failed");
        return -1;
    }

    double rate = (double)((double)(dot_clock * 1000.0) /
                           (double)(mode_line.htotal * mode_line.vtotal));

    // Assume 60Hz if we can't otherwise determine it.
    if (rate == 0)
        rate = 60;

    rate = 1000000.0 / rate;

    return (int)rate;
}

/**
 * \fn VideoOutputXv::ResizeForVideo(uint width, uint height)
 * Sets display parameters based on video resolution. 
 *
 * If we are using DisplayRes support we use the video size to
 * determine the desired screen size and refresh rate.
 * If we are also not using "GuiSizeForTV" we also resize
 * the video output window.
 *
 * \param width,height Resolution of the video we will be playing
 */
void VideoOutputXv::ResizeForVideo(uint width, uint height)
{
    if (width == 1920 && height == 1088)
        height = 1080; // ATSC 1920x1080

    if (display_res && display_res->SwitchToVideo(width, height))
    {
        // Switching to custom display resolution succeeded
        // Make a note of the new size
        w_mm = display_res->GetPhysicalWidth();
        h_mm = display_res->GetPhysicalHeight();
        display_aspect = display_res->GetAspectRatio();

        bool fullscreen = !gContext->GetNumSetting("GuiSizeForTV", 0);
        
        // if width && height are zero users expect fullscreen playback
        if (!fullscreen)
        {
            int gui_width = 0, gui_height = 0;
            gContext->GetResolutionSetting("Gui", gui_width, gui_height);
            fullscreen |= (0 == gui_width && 0 == gui_height);
        }

        if (fullscreen)
        {
            dispx = dispy = 0;
            dispw = display_res->GetWidth();
            disph = display_res->GetHeight();
            // Resize X window to fill new resolution
            X11S(XMoveResizeWindow(XJ_disp, XJ_win,
                                   dispx, dispy, dispw, disph));
        }
    }
}

/** 
 * \fn VideoOutputXv::InitDisplayMeasurements(uint width, uint height)
 * \brief Init display measurements based on database settings and
 *        actual screen parameters.
 */
void VideoOutputXv::InitDisplayMeasurements(uint width, uint height)
{
    if (display_res)
    {
        // The very first Resize needs to be the maximum possible
        // desired res, because X will mask off anything outside
        // the initial dimensions
        X11S(XMoveResizeWindow(XJ_disp, XJ_win, 0, 0,
                               display_res->GetMaxWidth(),
                               display_res->GetMaxHeight()));
        ResizeForVideo(width, height);
    }
    else
    {
        w_mm = (myth_dsw != 0) ?
            myth_dsw : DisplayWidthMM(XJ_disp, XJ_screen_num);

        h_mm = (myth_dsh != 0) ?
            myth_dsh : DisplayHeightMM(XJ_disp, XJ_screen_num);

        // Get default (possibly user selected) screen resolution from context
        float wmult, hmult;
        gContext->GetScreenSettings(XJ_screenx, XJ_screenwidth, wmult,
                                    XJ_screeny, XJ_screenheight, hmult);
    }

    // Fetch pixel width and height of the display
    int xbase, ybase, w, h;
    gContext->GetScreenBounds(xbase, ybase, w, h);

    // Determine window dimensions in pixels
    int window_w = w, window_h = h;
    if (gContext->GetNumSetting("GuiSizeForTV", 0))
        gContext->GetResolutionSetting("Gui", window_w,  window_h);
    else
        gContext->GetScreenBounds(xbase, ybase, window_w, window_h);
    window_w = (window_w) ? window_w : w;
    window_h = (window_h) ? window_h : h;
    float pixel_aspect = ((float)w) / ((float)h);

    VERBOSE(VB_PLAYBACK, LOC + QString(
                "Pixel dimensions: Screen %1x%2, window %3x%4")
            .arg(w).arg(h).arg(window_w).arg(window_h));

    // Determine if we are using Xinerama
    int event_base, error_base;
    bool usingXinerama = false;
    X11S(usingXinerama = 
         (XineramaQueryExtension(XJ_disp, &event_base, &error_base) &&
          XineramaIsActive(XJ_disp)));

    // If the dimensions are invalid, assume square pixels and 17" screen.
    // Only print warning if this isn't Xinerama, we will fix Xinerama later.
    if ((!h_mm || !w_mm) && !usingXinerama)
    {
        VERBOSE(VB_GENERAL, LOC + "Physical size of display unknown."
                "\n\t\t\tAssuming 17\" monitor with square pixels.");
    }

    h_mm = (h_mm) ? h_mm : 300;
    w_mm = (w_mm) ? w_mm : (int) round(h_mm * pixel_aspect);

    // If we are using Xinerama the display dimensions can not be trusted.
    // We need to use the Xinerama monitor aspect ratio from the DB to set
    // the physical screen width. This assumes the height is correct, which
    // is more or less true in the typical side-by-side monitor setup.
    if (usingXinerama)
    {
        float displayAspect = gContext->GetFloatSettingOnHost(
            "XineramaMonitorAspectRatio",
            gContext->GetHostName(), pixel_aspect);
        w_mm = (int) round(h_mm * displayAspect);
    }

    VERBOSE(VB_PLAYBACK, LOC + QString("Estimated display dimensions: "
                                       "%1x%2 mm Aspect: %3")
            .arg(w_mm).arg(h_mm).arg(((float)w_mm) / ((float)h_mm)));

    // We must now scale the display measurements to our window size.
    // If we are running fullscreen this is a no-op.
    w_mm = (w_mm * window_w) / w;
    h_mm = (h_mm * window_h) / h;

    // Now that we know the physical monitor size, we can
    // calculate the display aspect ratio pretty simply...
    display_aspect = ((float)w_mm) / ((float)h_mm);

    VERBOSE(VB_PLAYBACK, LOC + QString("Estimated window dimensions: "
                                       "%1x%2 mm Aspect: %3")
            .arg(w_mm).arg(h_mm).arg(display_aspect));

    // If we are using XRandR, use the aspect ratio from it instead...
    if (display_res)
        display_aspect = display_res->GetAspectRatio();
}

/**
 * \fn VideoOutputXv::GrabSuitableXvPort(Display*,Window,MythCodecID,uint,uint,int,XvMCSurfaceInfo*)
 * Internal function used to grab a XVideo port with the desired properties.
 *
 * \return port number if it succeeds, else -1.
 */
int VideoOutputXv::GrabSuitableXvPort(Display* disp, Window root,
                                      MythCodecID mcodecid,
                                      uint width, uint height,
                                      int xvmc_chroma,
                                      XvMCSurfaceInfo* xvmc_surf_info)
{
    uint neededFlags[] = { XvInputMask,
                           XvInputMask,
                           XvInputMask,
                           XvInputMask | XvImageMask };
    bool useXVMC[] = { true,  true,  true,  false };
    bool useVLD[]  = { true,  false, false, false };
    bool useIDCT[] = { false, true,  false, false };

    // avoid compiler warnings
    (void)width; (void)height; (void)xvmc_chroma; (void)xvmc_surf_info;
    (void)useVLD[0]; (void)useIDCT[0];

    QString msg[] =
    {
        "XvMC surface found with VLD support on port %1",
        "XvMC surface found with IDCT support on port %1",
        "XvMC surface found with MC support on port %1",
        "XVideo surface found on port %1"
    };

    // get the list of Xv ports
    XvAdaptorInfo *ai = NULL;
    uint p_num_adaptors = 0;
    int ret = Success;
    X11S(ret = XvQueryAdaptors(disp, root, &p_num_adaptors, &ai));
    if (Success != ret) 
    {
        VERBOSE(VB_IMPORTANT, LOC +
                "XVideo supported, but no free Xv ports found."
                "\n\t\t\tYou may need to reload video driver.");
        return -1;
    }

    // find an Xv port
    int port = -1, stream_type = 0;
    uint begin = 0, end = 4;
    switch (mcodecid)
    {
        case kCodec_MPEG1_XVMC: (stream_type = 1),(begin = 2),(end = 3); break;
        case kCodec_MPEG2_XVMC: (stream_type = 2),(begin = 2),(end = 3); break;
        case kCodec_H263_XVMC:  (stream_type = 3),(begin = 2),(end = 3); break;
        case kCodec_MPEG4_XVMC: (stream_type = 4),(begin = 2),(end = 3); break;

        case kCodec_MPEG1_IDCT: (stream_type = 1),(begin = 1),(end = 2); break;
        case kCodec_MPEG2_IDCT: (stream_type = 2),(begin = 1),(end = 2); break;
        case kCodec_H263_IDCT:  (stream_type = 3),(begin = 1),(end = 2); break;
        case kCodec_MPEG4_IDCT: (stream_type = 4),(begin = 1),(end = 2); break;

        case kCodec_MPEG1_VLD:  (stream_type = 1),(begin = 0),(end = 1); break;
        case kCodec_MPEG2_VLD:  (stream_type = 2),(begin = 0),(end = 1); break;
        case kCodec_H263_VLD:   (stream_type = 3),(begin = 0),(end = 1); break;
        case kCodec_MPEG4_VLD:  (stream_type = 4),(begin = 0),(end = 1); break;

        default:
            begin = 3; end = 4;
            break;
    }

    for (uint j = begin; j < end; ++j)
    {
        VERBOSE(VB_PLAYBACK, LOC + QString("@ j=%1 Looking for flag[s]: %2")
                .arg(j).arg(xvflags2str(neededFlags[j])));

        for (uint i = 0; i < p_num_adaptors && (port == -1); ++i) 
        {
            VERBOSE(VB_PLAYBACK, LOC + QString("Adaptor: %1 has flag[s]: %2")
                    .arg(i).arg(xvflags2str(ai[i].type)));

            if ((ai[i].type & neededFlags[j]) != neededFlags[j])
                continue;
            
            const XvPortID firstPort = ai[i].base_id;
            const XvPortID lastPort = ai[i].base_id + ai[i].num_ports - 1;
            XvPortID p = 0;
            if (useXVMC[j])
            {
#ifdef USING_XVMC
                int surfNum;
                XvMCSurfaceTypes::find(width, height, xvmc_chroma,
                                       useVLD[j], useIDCT[j], stream_type,
                                       0, 0,
                                       disp, firstPort, lastPort,
                                       p, surfNum);
                if (surfNum<0)
                    continue;
                
                XvMCSurfaceTypes surf(disp, p);
                
                if (!surf.size())
                    continue;

                X11L;
                ret = XvGrabPort(disp, p, CurrentTime);
                if (Success == ret)
                {
                    VERBOSE(VB_PLAYBACK, LOC + "Grabbed xv port "<<p);
                    port = p;
                    add_open_xv_port(disp, p);
                }
                X11U;
                if (Success != ret)
                {
                    VERBOSE(VB_PLAYBACK,  LOC + "Failed to grab xv port "<<p);
                    continue;
                }
                
                if (xvmc_surf_info)
                    surf.set(surfNum, xvmc_surf_info);
#endif // USING_XVMC
            }
            else
            {
                for (p = firstPort; (p <= lastPort) && (port == -1); ++p)
                {
                    X11L;
                    ret = XvGrabPort(disp, p, CurrentTime);
                    if (Success == ret)
                    {
                        VERBOSE(VB_PLAYBACK,  LOC + "Grabbed xv port "<<p);
                        port = p;
                        add_open_xv_port(disp, p);
                    }
                    X11U;
                }
            }
        }
        if (port != -1)
        {
            VERBOSE(VB_PLAYBACK, LOC + msg[j].arg(port));
            break;
        }
    }
    if (port == -1)
        VERBOSE(VB_PLAYBACK, LOC + "No suitible XVideo port found");

    // free list of Xv ports
    if (ai)
        X11S(XvFreeAdaptorInfo(ai));

    return port;
}

/**
 * \fn VideoOutputXv::CreatePauseFrame(void)
 * Creates an extra frame for pause.
 * 
 * This creates a pause frame by copies the scratch frame settings, a
 * and allocating a databuffer, so a scratch must already exist.
 * XvMC does not use this pause frame facility so this only creates
 * a pause buffer for the other output methods.
 *
 * \sideeffect sets av_pause_frame.
 */
void VideoOutputXv::CreatePauseFrame(void)
{
    // All methods but XvMC use a pause frame, create it if needed
    if (VideoOutputSubType() <= XVideo)
    {
        vbuffers.LockFrame(&av_pause_frame, "CreatePauseFrame");

        if (av_pause_frame.buf)
        {
            delete [] av_pause_frame.buf;
            av_pause_frame.buf = NULL;
        }
        av_pause_frame.height       = vbuffers.GetScratchFrame()->height;
        av_pause_frame.width        = vbuffers.GetScratchFrame()->width;
        av_pause_frame.bpp          = vbuffers.GetScratchFrame()->bpp;
        av_pause_frame.size         = vbuffers.GetScratchFrame()->size;
        av_pause_frame.frameNumber  = vbuffers.GetScratchFrame()->frameNumber;
        av_pause_frame.buf          = new unsigned char[av_pause_frame.size];
        av_pause_frame.qscale_table = NULL;
        av_pause_frame.qstride      = 0;

        vbuffers.UnlockFrame(&av_pause_frame, "CreatePauseFrame");
    }
}

/**
 * \fn VideoOutputXv::InitVideoBuffers(MythCodecID,bool,bool)
 * Creates and initializes video buffers.
 *
 * \sideeffect sets video_output_subtype if it succeeds.
 *
 * \bug Extra buffers are pre-allocated here for XVMC_VLD
 *      due to a bug somewhere else, see comment in code.
 *
 * \return success or failure at creating any buffers.
 */
bool VideoOutputXv::InitVideoBuffers(MythCodecID mcodecid,
                                     bool use_xv, bool use_shm)
{
    (void)mcodecid;

    bool done = false;
    // If use_xvmc try to create XvMC buffers
#ifdef USING_XVMC
    if (mcodecid > kCodec_NORMAL_END)
    {
        // Create ffmpeg VideoFrames    
        bool vld, idct, mc;
        myth2av_codecid(myth_codec_id, vld, idct, mc);

        if (vld)
            xvmc_buf_attr->SetNumSurf(16);

        vbuffers.Init(xvmc_buf_attr->GetNumSurf(),
                      false /* create an extra frame for pause? */,
                      xvmc_buf_attr->GetFrameReserve(),
                      xvmc_buf_attr->GetPreBufferGoal(),
                      xvmc_buf_attr->GetPreBufferGoal(),
                      xvmc_buf_attr->GetNeededBeforeDisplay(),
                      true /*use_frame_locking*/);
        
        
        done = InitXvMC(mcodecid);

        if (!done)
            vbuffers.Reset();
    }
#endif // USING_XVMC

    // Create ffmpeg VideoFrames    
    if (!done)
        vbuffers.Init(31, true, 1, 12, 4, 2, false);

    // Fall back to XVideo if there is an xv_port
    if (!done && use_xv)
        done = InitXVideo();

    // Fall back to shared memory, if we are allowed to use it
    if (!done && use_shm)
        done = InitXShm();
 
    // Fall back to plain old X calls
    if (!done)
        done = InitXlib();

    // XVideo & XvMC output methods allow the picture to be adjusted
    if (done && VideoOutputSubType() >= XVideo &&
        gContext->GetNumSetting("UseOutputPictureControls", 0))
    {
        ChangePictureAttribute(kPictureAttribute_Brightness, brightness);
        ChangePictureAttribute(kPictureAttribute_Contrast, contrast);
        ChangePictureAttribute(kPictureAttribute_Colour, colour);
        ChangePictureAttribute(kPictureAttribute_Hue, hue);
    }

    return done;
}

/**
 * \fn VideoOutputXv::InitXvMC(MythCodecID)
 *  Creates and initializes video buffers.
 *
 * \sideeffect sets video_output_subtype if it succeeds.
 *
 * \return success or failure at creating any buffers.
 */
bool VideoOutputXv::InitXvMC(MythCodecID mcodecid)
{
    (void)mcodecid;
#ifdef USING_XVMC
    xv_port = GrabSuitableXvPort(XJ_disp, XJ_root, mcodecid,
                                 XJ_width, XJ_height, xvmc_chroma,
                                 &xvmc_surf_info);
    if (xv_port == -1)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                "Could not find suitable XvMC surface.");
        return false;
    }

    InstallXErrorHandler(XJ_disp);

    // create XvMC buffers
    bool ok = CreateXvMCBuffers();
    vector<XErrorEvent> errs = UninstallXErrorHandler(XJ_disp);
    if (!ok || errs.size())
    {
        PrintXErrors(XJ_disp, errs);
        DeleteBuffers(XVideoMC, false);
        ok = false;
    }

    if (ok)
    {
        video_output_subtype = XVideoMC;
        if (XVMC_IDCT == (xvmc_surf_info.mc_type & XVMC_IDCT))
            video_output_subtype = XVideoIDCT; 
        if (XVMC_VLD == (xvmc_surf_info.mc_type & XVMC_VLD))
            video_output_subtype = XVideoVLD;
    }
    else
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Failed to create XvMC Buffers.");

        xvmc_osd_lock.lock();
        for (uint i=0; i<xvmc_osd_available.size(); i++)
            delete xvmc_osd_available[i];
        xvmc_osd_available.clear();
        xvmc_osd_lock.unlock();
        VERBOSE(VB_PLAYBACK, LOC + "Closing XVideo port " << xv_port);
        X11L;
        XvUngrabPort(XJ_disp, xv_port, CurrentTime);
        del_open_xv_port(xv_port);
        X11U;
        xv_port = -1;
    }

    return ok;
#else // USING_XVMC
    return false;
#endif // USING_XVMC
}

/**
 * \fn VideoOutputXv::InitXVideo()
 * Creates and initializes video buffers.
 *
 * \sideeffect sets video_output_subtype if it succeeds.
 *
 * \return success or failure at creating any buffers.
 */
bool VideoOutputXv::InitXVideo()
{
    xv_port = GrabSuitableXvPort(XJ_disp, XJ_root, kCodec_MPEG2,
                                 XJ_width, XJ_height);
    if (xv_port == -1)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                "Could not find suitable XVideo surface.");
        return false;
    }

    InstallXErrorHandler(XJ_disp);

    bool foundimageformat = false;
    int formats = 0;
    XvImageFormatValues *fo;
    X11S(fo = XvListImageFormats(XJ_disp, xv_port, &formats));
    for (int i = 0; i < formats; i++)
    {
        if ((fo[i].id == GUID_I420_PLANAR) ||
            (fo[i].id == GUID_IYUV_PLANAR))
        {
            foundimageformat = true;
            xv_chroma = GUID_I420_PLANAR;
        }
    }

    if (!foundimageformat)
    {
        for (int i = 0; i < formats; i++)
        {
            if (fo[i].id == GUID_YV12_PLANAR)
            {
                foundimageformat = true;
                xv_chroma = GUID_YV12_PLANAR;
            }
        }
    }

    for (int i = 0; i < formats; i++)
    {
        char *chr = (char*) &(fo[i].id);
        VERBOSE(VB_PLAYBACK, LOC + QString("XVideo Format #%1 is '%2%3%4%5'")
                .arg(i).arg(chr[0]).arg(chr[1]).arg(chr[2]).arg(chr[3]));
    }

    if (fo)
        X11S(XFree(fo));

    if (foundimageformat)
    {
        char *chr = (char*) &xv_chroma;
        VERBOSE(VB_PLAYBACK, LOC + QString("Using XVideo Format '%1%2%3%4'")
                .arg(chr[0]).arg(chr[1]).arg(chr[2]).arg(chr[3]));
    }
    else
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                "Couldn't find the proper XVideo image format.");
        VERBOSE(VB_PLAYBACK, LOC + "Closing XVideo port " << xv_port);
        X11L;
        XvUngrabPort(XJ_disp, xv_port, CurrentTime);
        del_open_xv_port(xv_port);
        X11U;
        xv_port = -1;
    }

    bool ok = xv_port >= 0;
    if (ok)
        ok = CreateBuffers(XVideo);

    vector<XErrorEvent> errs = UninstallXErrorHandler(XJ_disp);
    if (!ok || errs.size())
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Failed to create XVideo Buffers.");
        DeleteBuffers(XVideo, false);
        VERBOSE(VB_PLAYBACK, LOC + "Closing XVideo port " << xv_port);
        X11L;
        XvUngrabPort(XJ_disp, xv_port, CurrentTime);
        del_open_xv_port(xv_port);
        X11U;
        xv_port = -1;
        ok = false;
    }
    else
        video_output_subtype = XVideo;
    
    return ok;
}

/**
 * \fn VideoOutputXv::InitXShm()
 * Creates and initializes video buffers.
 *
 * \sideeffect sets video_output_subtype if it succeeds.
 *
 * \return success or failure at creating any buffers.
 */
bool VideoOutputXv::InitXShm()
{
    InstallXErrorHandler(XJ_disp);

    VERBOSE(VB_IMPORTANT, LOC +
            "Falling back to X shared memory video output."
            "\n\t\t\t      *** May be slow ***");

    bool ok = CreateBuffers(XShm);

    vector<XErrorEvent> errs = UninstallXErrorHandler(XJ_disp);
    if (!ok || errs.size())
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Failed to allocate X shared memory.");
        PrintXErrors(XJ_disp, errs);
        DeleteBuffers(XShm, false);
        ok = false;
    }
    else
        video_output_subtype = XShm;
    
    return ok;
}

/**
 * \fn VideoOutputXv::InitXlib()
 * Creates and initializes video buffers.
 *
 * \sideeffect sets video_output_subtype if it succeeds.
 *
 * \return success or failure at creating any buffers.
 */
bool VideoOutputXv::InitXlib()
{ 
    InstallXErrorHandler(XJ_disp);

    VERBOSE(VB_IMPORTANT, LOC +
            "Falling back to X11 video output over a network socket."
            "\n\t\t\t      *** May be very slow ***");

    bool ok = CreateBuffers(Xlib);

    vector<XErrorEvent> errs = UninstallXErrorHandler(XJ_disp);
    if (!ok || errs.size())
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Failed to create X buffers.");
        PrintXErrors(XJ_disp, errs);
        DeleteBuffers(Xlib, false);
        ok = false;
    }
    else
        video_output_subtype = Xlib;

    return ok;
}

/** \fn VideoOutputXv::GetBestSupportedCodec(uint,uint,uint,uint,uint,int,bool)
 *
 *  \return MythCodecID for the best supported codec on the main display.
 */
MythCodecID VideoOutputXv::GetBestSupportedCodec(
    uint width,       uint height,
    uint osd_width,   uint osd_height,
    uint stream_type, int xvmc_chroma,    bool test_surface)
{
    (void)width, (void)height, (void)osd_width, (void)osd_height;
    (void)stream_type, (void)xvmc_chroma, (void)test_surface;

#ifdef USING_XVMC
    Display *disp;
    X11S(disp = XOpenDisplay(NULL));

    // Disable features based on environment and DB values.
    bool use_xvmc_vld = false, use_xvmc_idct = false, use_xvmc = false;
    bool use_xv = true, use_shm = true;

    QString dec = gContext->GetSetting("PreferredMPEG2Decoder", "ffmpeg");
    if (dec == "xvmc")
        use_xvmc_idct = use_xvmc = true;
    else if (dec == "xvmc-vld")
        use_xvmc_vld = use_xvmc = true;

    SetFromEnv(use_xvmc_vld, use_xvmc_idct, use_xvmc, use_xv, use_shm);
    SetFromHW(disp, use_xvmc, use_xv, use_shm);

    MythCodecID ret = (MythCodecID)(kCodec_MPEG1 + (stream_type-1));
    if (use_xvmc_vld &&
        XvMCSurfaceTypes::has(disp, XvVLD, stream_type, xvmc_chroma,
                              width, height, osd_width, osd_height))
    {
        ret = (MythCodecID)(kCodec_MPEG1_VLD + (stream_type-1));
    }
    else if (use_xvmc_idct &&
        XvMCSurfaceTypes::has(disp, XvIDCT, stream_type, xvmc_chroma,
                              width, height, osd_width, osd_height))
    {
        ret = (MythCodecID)(kCodec_MPEG1_IDCT + (stream_type-1));
    }
    else if (use_xvmc &&
             XvMCSurfaceTypes::has(disp, XvMC, stream_type, xvmc_chroma,
                                   width, height, osd_width, osd_height))
    {
        ret = (MythCodecID)(kCodec_MPEG1_XVMC + (stream_type-1));
    }

    bool ok = true;
    if (test_surface && ret>kCodec_NORMAL_END)
    {
        Window root;
        XvMCSurfaceInfo info;

        ok = false;
        X11S(root = DefaultRootWindow(disp));
        int port = GrabSuitableXvPort(disp, root, ret, width, height,
                                      xvmc_chroma, &info);
        if (port>=0)
        {
            XvMCContext *ctx =
                CreateXvMCContext(disp, port, info.surface_type_id,
                                  width, height);
            ok = NULL != ctx;
            DeleteXvMCContext(disp, ctx);
            VERBOSE(VB_PLAYBACK, LOC + "Closing XVideo port " << port);
            X11L;
            XvUngrabPort(disp, port, CurrentTime);
            del_open_xv_port(port);
            X11U;
        }
    }
    X11S(XCloseDisplay(disp));
    X11S(ok |= cnt_open_xv_port() > 0); // also ok if we already opened port..

    if (!ok)
    {
        QString msg = LOC_ERR + "Could not open XvMC port...\n"
                "\n"
                "\t\t\tYou may wish to verify that your DISPLAY\n"
                "\t\t\tenvironment variable does not use an external\n"
                "\t\t\tnetwork connection.\n";
#ifdef USING_XVMCW
        msg +=  "\n"
                "\t\t\tYou may also wish to verify that\n"
                "\t\t\t/etc/X11/XvMCConfig contains the correct\n"
                "\t\t\tvendor's XvMC library.\n";
#endif // USING_XVMCW
        VERBOSE(VB_IMPORTANT, msg);
        ret = (MythCodecID)(kCodec_MPEG1 + (stream_type-1));
    }

    return ret;
#else // if !USING_XVMC
    return (MythCodecID)(kCodec_MPEG1 + (stream_type-1));
#endif // !USING_XVMC
}

#define XV_INIT_FATAL_ERROR_TEST(test,msg) \
do { \
    if (test) \
    { \
        VERBOSE(VB_IMPORTANT, LOC_ERR + msg << " Exiting playback."); \
        errored = true; \
        return false; \
    } \
} while (false)

/**
 * \fn VideoOutputXv::Init(int,int,float,WId,int,int,int,int,WId)
 * Initializes class for video output.
 *
 * \return success or failure.
 */
bool VideoOutputXv::Init(
    int width, int height, float aspect, 
    WId winid, int winx, int winy, int winw, int winh, WId embedid)
{
    needrepaint = true;

    XV_INIT_FATAL_ERROR_TEST(winid <= 0, "Invalid Window ID.");

    X11S(XJ_disp = XOpenDisplay(NULL));
    XV_INIT_FATAL_ERROR_TEST(!XJ_disp, "Failed to open display.");

    // Initialize X stuff
    X11L;
    XJ_screen     = DefaultScreenOfDisplay(XJ_disp);
    XJ_screen_num = DefaultScreen(XJ_disp);
    XJ_white      = XWhitePixel(XJ_disp, XJ_screen_num);
    XJ_black      = XBlackPixel(XJ_disp, XJ_screen_num);
    XJ_curwin     = winid;
    XJ_win        = winid;
    XJ_root       = DefaultRootWindow(XJ_disp);
    XJ_gc         = XCreateGC(XJ_disp, XJ_win, 0, 0);
    XJ_depth      = DefaultDepthOfScreen(XJ_screen);
    X11U;

    // Basic setup
    VideoOutput::Init(width, height, aspect,
                      winid, winx, winy, winw, winh,
                      embedid);

    // Set resolution/measurements (check XRandR, Xinerama, config settings)
    InitDisplayMeasurements(width, height);

    // Set use variables...
    bool vld, idct, mc, xv, shm;
    myth2av_codecid(myth_codec_id, vld, idct, mc);
    xv = shm = !vld && !idct;
    SetFromEnv(vld, idct, mc, xv, shm);
    SetFromHW(XJ_disp, mc, xv, shm);
    bool use_chroma_key_osd = gContext->GetNumSettingOnHost(
        "UseChromaKeyOSD", gContext->GetHostName(), 0);
    use_chroma_key_osd &= (xv || vld || idct || mc);

    // Set embedding window id
    if (embedid > 0)
        XJ_curwin = XJ_win = embedid;

    // create chroma key osd structure if needed
    if (use_chroma_key_osd && ((32 == XJ_depth) || (24 == XJ_depth)))
    {
        chroma_osd = new ChromaKeyOSD(this);
#ifdef USING_XVMC
        xvmc_buf_attr->SetOSDNum(0); // disable XvMC blending OSD
#endif // USING_XVMC
    }
    else if (use_chroma_key_osd)
    {
        VERBOSE(VB_IMPORTANT, LOC + QString(
                    "Number of bits per pixel is %1, \n\t\t\t"
                    "but we only support ARGB 32 bbp for ChromaKeyOSD.")
                .arg(XJ_depth));
    }

    // Create video buffers
    bool ok = InitVideoBuffers(myth_codec_id, xv, shm);
    XV_INIT_FATAL_ERROR_TEST(!ok, "Failed to get any video output");

    if (video_output_subtype >= XVideo)
        InitColorKey(true);

    // Deal with the nVidia 6xxx & 7xxx cards which do
    // not support chromakeying with the latest drivers
    if (!xv_colorkey && chroma_osd)
    {
        VERBOSE(VB_IMPORTANT, LOC + "Ack! Disabling ChromaKey OSD"
                "\n\t\t\tWe can't use ChromaKey OSD "
                "if chromakeying is not supported!");

#ifdef USING_XVMC
        // Delete the buffers we allocated before
        DeleteBuffers(VideoOutputSubType(), true);
        if (xv_port >= 0)
        {
            VERBOSE(VB_PLAYBACK, LOC + "Closing XVideo port " << xv_port);
            X11L;
            XvUngrabPort(XJ_disp, xv_port, CurrentTime);
            del_open_xv_port(xv_port);
            X11U;
            xv_port = -1;
        }
#endif // USING_XVMC

        // Get rid of the chromakey osd..
        delete chroma_osd;
        chroma_osd = NULL;

#ifdef USING_XVMC
        // Recreate video buffers
        xvmc_buf_attr->SetOSDNum(1);
        ok = InitVideoBuffers(myth_codec_id, xv, shm);
        XV_INIT_FATAL_ERROR_TEST(!ok, "Failed to get any video output (nCK)");
#endif // USING_XVMC
    }

    MoveResize(); 

    XJ_started = true;

    return true;
}
#undef XV_INIT_FATAL_ERROR_TEST

/**
 * \fn VideoOutputXv::InitColorKey(bool)
 * Initializes color keying support used by XVideo output methods.
 *
 * \param turnoffautopaint turn off or on XV_AUTOPAINT_COLORKEY property.
 */
void VideoOutputXv::InitColorKey(bool turnoffautopaint)
{
    int ret = Success, xv_val=0;
    xv_draw_colorkey = true;
    xv_colorkey = 0; // set to invalid value as a sentinel

    Atom xv_atom;
    XvAttribute *attributes;
    int attrib_count;

    X11S(attributes = XvQueryPortAttributes(XJ_disp, xv_port, &attrib_count));
    for (int i = (attributes) ? 0 : attrib_count; i < attrib_count; i++)
    {
        if (!strcmp(attributes[i].name, "XV_AUTOPAINT_COLORKEY"))
        {
            X11S(xv_atom = XInternAtom(XJ_disp, "XV_AUTOPAINT_COLORKEY", False));
            if (xv_atom == None)
                continue;

            X11L;
            if (turnoffautopaint)
                ret = XvSetPortAttribute(XJ_disp, xv_port, xv_atom, 0);
            else
                ret = XvSetPortAttribute(XJ_disp, xv_port, xv_atom, 1);

            ret = XvGetPortAttribute(XJ_disp, xv_port, xv_atom, &xv_val);
            // turn of colorkey drawing if autopaint is on
            if (Success == ret && xv_val)
                xv_draw_colorkey = false;
            X11U;
        }
    }
    if (attributes)
        X11S(XFree(attributes));

    if (xv_draw_colorkey)
    {
        X11S(xv_atom = XInternAtom(XJ_disp, "XV_COLORKEY", False));
        if (xv_atom != None)
        {
            X11S(ret = XvGetPortAttribute(XJ_disp, xv_port, xv_atom, 
                                          &xv_colorkey));

            if (ret == Success && xv_colorkey == 0)
            {
                const int default_colorkey = 1;
                X11S(ret = XvSetPortAttribute(XJ_disp, xv_port, xv_atom,
                                              default_colorkey));
                if (ret == Success)
                {
                    VERBOSE(VB_PLAYBACK, LOC +
                            "0,0,0 is the only bad color key for MythTV, "
                            "using "<<default_colorkey<<" instead.");
                    xv_colorkey = default_colorkey;
                }
                ret = Success;
            }

            if (ret != Success)
            {
                VERBOSE(VB_IMPORTANT, LOC_ERR +
                        "Couldn't get the color key color,"
                        "\n\t\t\tprobably due to a driver bug or limitation."
                        "\n\t\t\tYou might not get any video, "
                        "but we'll try anyway.");
                xv_colorkey = 0;
            }
        }
    }
}

bool VideoOutputXv::SetupDeinterlace(bool interlaced,
                                     const QString& overridefilter)
{
    QString f = (VideoOutputSubType() > XVideo) ? "bobdeint" : overridefilter;
    bool deint = VideoOutput::SetupDeinterlace(interlaced, f);
    needrepaint = true;
    return deint;
}

/**
 * \fn VideoOutput::NeedsDoubleFramerate() const
 * Approves bobdeint filter for XVideo and XvMC surfaces,
 * rejects other filters for XvMC, and defers to
 * VideoOutput::ApproveDeintFilter(const QString&)
 * otherwise.
 *
 * \return whether current video output supports a specific filter.
 */
bool VideoOutputXv::ApproveDeintFilter(const QString& filtername) const
{
    // TODO implement bobdeint for non-Xv[MC]
    VOSType vos = VideoOutputSubType();
    if (filtername == "bobdeint" && vos >= XVideo)
        return true;
    else if (vos > XVideo)
        return false;
    else
        return VideoOutput::ApproveDeintFilter(filtername);
}

#ifdef USING_XVMC
static uint calcBPM(int chroma)
{
    int ret;
    switch (chroma)
    {
        case XVMC_CHROMA_FORMAT_420: ret = 6;
        case XVMC_CHROMA_FORMAT_422: ret = 4+2;
        case XVMC_CHROMA_FORMAT_444: ret = 4+4;
        default: ret = 6;
        // default unless gray, then 4 is the right number,
        // a bigger number just wastes a little memory.
    }
    return ret;
}
#endif

XvMCContext* VideoOutputXv::CreateXvMCContext(
    Display* disp, int port, int surf_type, int width, int height)
{
    (void)disp; (void)port; (void)surf_type; (void)width; (void)height;
#ifdef USING_XVMC
    int ret = Success;
    XvMCContext *ctx = new XvMCContext;
    X11S(ret = XvMCCreateContext(disp, port, surf_type, width, height,
                                 XVMC_DIRECT, ctx));
    if (ret != Success)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                QString("Unable to create XvMC Context, status(%1): %2")
                .arg(ret).arg(ErrorStringXvMC(ret)));

        delete ctx;
        ctx = NULL;
    }
    return ctx;
#else // if !USING_XVMC 
    return NULL;
#endif // !USING_XVMC
}

void VideoOutputXv::DeleteXvMCContext(Display* disp, XvMCContext*& ctx)
{
    (void)disp; (void)ctx;
#ifdef USING_XVMC
    if (ctx)
    {
        X11S(XvMCDestroyContext(disp, ctx));
        delete ctx;
        ctx = NULL;
    }
#endif // !USING_XVMC
}

bool VideoOutputXv::CreateXvMCBuffers(void)
{
#ifdef USING_XVMC
    xvmc_ctx = CreateXvMCContext(XJ_disp, xv_port,
                                 xvmc_surf_info.surface_type_id,
                                 XJ_width, XJ_height);
    if (!xvmc_ctx)
        return false;

    bool createBlocks = !(XVMC_VLD == (xvmc_surf_info.mc_type & XVMC_VLD));
    xvmc_surfs = CreateXvMCSurfaces(xvmc_buf_attr->GetMaxSurf(), createBlocks);
    if (xvmc_surfs.size() < xvmc_buf_attr->GetMinSurf())
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Unable to create XvMC Surfaces");
        DeleteBuffers(XVideoMC, false);
        return false;
    }

    bool ok = vbuffers.CreateBuffers(XJ_width, XJ_height, XJ_disp, xvmc_ctx,
                                     &xvmc_surf_info, xvmc_surfs);
    if (!ok)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Unable to create XvMC Buffers");
        DeleteBuffers(XVideoMC, false);
        return false;
    }

    xvmc_osd_lock.lock();
    for (uint i=0; i < xvmc_buf_attr->GetOSDNum(); i++)
    {
        XvMCOSD *xvmc_osd =
            new XvMCOSD(XJ_disp, xv_port, xvmc_surf_info.surface_type_id,
                        xvmc_surf_info.flags);
        xvmc_osd->CreateBuffer(*xvmc_ctx, XJ_width, XJ_height);
        xvmc_osd_available.push_back(xvmc_osd);
    }
    xvmc_osd_lock.unlock();


    X11S(XSync(XJ_disp, False));

    return true;
#else
    return false;
#endif // USING_XVMC
}

vector<void*> VideoOutputXv::CreateXvMCSurfaces(uint num, bool create_xvmc_blocks)
{
    (void)num;
    (void)create_xvmc_blocks;

    vector<void*> surfaces;
#ifdef USING_XVMC
    uint blocks_per_macroblock = calcBPM(xvmc_chroma);
    uint num_mv_blocks   = ((XJ_width + 15) / 16) * ((XJ_height + 15) / 16);
    uint num_data_blocks = num_mv_blocks * blocks_per_macroblock;

    // create needed XvMC stuff
    bool ok = true;
    for (uint i = 0; i < num; i++)
    {
        xvmc_vo_surf_t *surf = new xvmc_vo_surf_t;
        bzero(surf, sizeof(xvmc_vo_surf_t));

        X11L;

        int ret = XvMCCreateSurface(XJ_disp, xvmc_ctx, &(surf->surface));
        ok &= (Success == ret);

        if (create_xvmc_blocks && ok)
        {
            ret = XvMCCreateBlocks(XJ_disp, xvmc_ctx, num_data_blocks,
                                   &(surf->blocks));
            if (Success != ret)
            {
                XvMCDestroySurface(XJ_disp, &(surf->surface));
                ok = false;
            }
        }

        if (create_xvmc_blocks && ok)
        {
            ret = XvMCCreateMacroBlocks(XJ_disp, xvmc_ctx, num_mv_blocks,
                                        &(surf->macro_blocks));
            if (Success != ret)
            {
                XvMCDestroyBlocks(XJ_disp, &(surf->blocks));
                XvMCDestroySurface(XJ_disp, &(surf->surface));
                ok = false;
            }
        }

        X11U;

        if (!ok)
        {
            delete surf;
            break;
        }
        surfaces.push_back(surf);
    }
#endif // USING_XVMC
    return surfaces;
}

/**
 * \fn VideoOutputXv::CreateShmImages(uint num, bool use_xv)
 * \brief Creates Shared Memory Images.
 *
 *  Each XvImage/XImage created is added to xv_buffers, and shared
 *  memory info is added to XJ_shm_infos.
 * 
 * \param  num      number of buffers to create
 * \param  use_xv   use XvShmCreateImage instead of XShmCreateImage
 * \return vector containing image data for each buffer created
 */
vector<unsigned char*> VideoOutputXv::CreateShmImages(uint num, bool use_xv)
{
    VERBOSE(VB_PLAYBACK, LOC + QString("CreateShmImages(%1): ").arg(num)
            <<QString("XJ: (%1,%2)").arg(XJ_width).arg(XJ_height));

    vector<unsigned char*> bufs;
    XShmSegmentInfo blank;
    // for now make reserve big enough to avoid realloc.. 
    // we should really have vector of pointers...
    XJ_shm_infos.reserve(max(num + 32, (uint)128));
    for (uint i = 0; i < num; i++)
    {
        XJ_shm_infos.push_back(blank);
        void *image = NULL;
        int size = 0;
        int desiredsize = 0;

        X11L;

        if (use_xv)
        {
            image = XvShmCreateImage(XJ_disp, xv_port, xv_chroma, 0, 
                                     XJ_width, XJ_height, &XJ_shm_infos[i]);
            size = ((XvImage*)image)->data_size + 64;
            desiredsize = XJ_width * XJ_height * 3 / 2;

            if (image && size < desiredsize)
            {
                VERBOSE(VB_IMPORTANT, LOC_ERR + "CreateXvShmImages(): "
                        "XvShmCreateImage() failed to create image of the "
                        "requested size.");
                XFree(image);
                image = NULL;
            }
        }
        else
        {
            XImage *img =
                XShmCreateImage(XJ_disp, DefaultVisual(XJ_disp, XJ_screen_num),
                                XJ_depth, ZPixmap, 0, &XJ_shm_infos[i],
                                dispw, disph);
            size = img->bytes_per_line * img->height + 64;
            image = img;
            desiredsize = dispw * disph * 3 / 2;
            if (image && size < desiredsize)
            {
                VERBOSE(VB_IMPORTANT, LOC_ERR + "CreateXvShmImages(): "
                        "XShmCreateImage() failed to create image of the "
                        "requested size.");
                XDestroyImage((XImage *)image);
                image = NULL;
            }
        }

        X11U;

        if (image)
        {
            XJ_shm_infos[i].shmid = shmget(IPC_PRIVATE, size, IPC_CREAT|0777);
            if (XJ_shm_infos[i].shmid >= 0)
            {
                XJ_shm_infos[i].shmaddr = (char*) shmat(XJ_shm_infos[i].shmid, 0, 0);
                if (use_xv)
                    ((XvImage*)image)->data = XJ_shm_infos[i].shmaddr;
                else
                    ((XImage*)image)->data = XJ_shm_infos[i].shmaddr;
                xv_buffers[(unsigned char*) XJ_shm_infos[i].shmaddr] = image;
                XJ_shm_infos[i].readOnly = False;

                X11L;
                XShmAttach(XJ_disp, &XJ_shm_infos[i]);
                XSync(XJ_disp, False); // needed for FreeBSD?
                X11U;

                // Mark for delete immediately.
                // It won't actually be removed until after we detach it.
                shmctl(XJ_shm_infos[i].shmid, IPC_RMID, 0);

                bufs.push_back((unsigned char*) XJ_shm_infos[i].shmaddr);
            }
            else
            { 
                VERBOSE(VB_IMPORTANT, LOC_ERR +
                        "CreateXvShmImages(): shmget() failed." + ENO);
                break;
            }
        }
        else
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + "CreateXvShmImages(): "
                    "XvShmCreateImage() failed to create image.");
            break;
        }
    }
    return bufs;
}

bool VideoOutputXv::CreateBuffers(VOSType subtype)
{
    bool ok = false;

    if (subtype > XVideo && xv_port >= 0)
        ok = CreateXvMCBuffers();
    else if (subtype == XVideo && xv_port >= 0)
    {
        vector<unsigned char*> bufs = 
            CreateShmImages(vbuffers.allocSize(), true);
        ok = vbuffers.CreateBuffers(XJ_width, XJ_height, bufs);

        clear_xv_buffers(vbuffers, XJ_width, XJ_height, xv_chroma);

        X11S(XSync(XJ_disp, False));
        if (xv_chroma != GUID_I420_PLANAR)
            xv_color_conv_buf = new unsigned char[XJ_width * XJ_height * 3 / 2];
    }
    else if (subtype == XShm || subtype == Xlib)
    {
        if (subtype == XShm)
        {
            CreateShmImages(1, false);
            XJ_non_xv_image = (XImage*) xv_buffers.begin()->second;
        }
        else
        {

            X11L;

            int bytes_per_line = XJ_depth / 8 * dispw;
            int scrn = DefaultScreen(XJ_disp);
            Visual *visual = DefaultVisual(XJ_disp, scrn);
            XJ_non_xv_image = XCreateImage(XJ_disp, visual, XJ_depth,
                                           ZPixmap, /*offset*/0, /*data*/0,
                                           dispw, disph, /*bitmap_pad*/0,
                                           bytes_per_line);

            X11U;

            if (!XJ_non_xv_image)
            {
                VERBOSE(VB_IMPORTANT, LOC_ERR + "XCreateImage failed: "
                        <<"XJ_disp("<<XJ_disp<<") visual("<<visual<<") "<<endl
                        <<"                        "
                        <<"XJ_depth("<<XJ_depth<<") "
                        <<"WxH("<<dispw<<"x"<<disph<<") "
                        <<"bpl("<<bytes_per_line<<")");
                return false;
            }
            XJ_non_xv_image->data = (char*) malloc(bytes_per_line * disph);
        }

        switch (XJ_non_xv_image->bits_per_pixel)
        {   // only allow these three output formats for non-xv videout
            case 16: non_xv_av_format = PIX_FMT_RGB565; break;
            case 24: non_xv_av_format = PIX_FMT_RGB24;  break;
            case 32: non_xv_av_format = PIX_FMT_RGBA32; break;
            default: non_xv_av_format = PIX_FMT_NB;
        }
        if (PIX_FMT_NB == non_xv_av_format)
        {
            QString msg = QString(
                "Non XVideo modes only support displays with 16,\n\t\t\t"
                "24, or 32 bits per pixel. But you have a %1 bpp display.")
                .arg(XJ_depth*8);
            
            VERBOSE(VB_IMPORTANT, LOC_ERR + msg);
        }
        else
            ok = vbuffers.CreateBuffers(XJ_width, XJ_height);

    }

    if (ok)
        CreatePauseFrame();

    return ok;
}

void VideoOutputXv::DeleteBuffers(VOSType subtype, bool delete_pause_frame)
{
    (void) subtype;
    DiscardFrames(true);

#ifdef USING_XVMC
    // XvMC buffers
    for (uint i=0; i<xvmc_surfs.size(); i++)
    {
        xvmc_vo_surf_t *surf = (xvmc_vo_surf_t*) xvmc_surfs[i];
        X11S(XvMCHideSurface(XJ_disp, &(surf->surface)));
    }
    DiscardFrames(true);
    for (uint i=0; i<xvmc_surfs.size(); i++)
    {
        xvmc_vo_surf_t *surf = (xvmc_vo_surf_t*) xvmc_surfs[i];

        X11L;

        XvMCDestroySurface(XJ_disp, &(surf->surface));
        XvMCDestroyMacroBlocks(XJ_disp, &(surf->macro_blocks));
        XvMCDestroyBlocks(XJ_disp, &(surf->blocks));

        X11U;
    }
    xvmc_surfs.clear();

    // OSD buffers
    xvmc_osd_lock.lock();
    for (uint i=0; i<xvmc_osd_available.size(); i++)
    {
        xvmc_osd_available[i]->DeleteBuffer();
        delete xvmc_osd_available[i];
    }
    xvmc_osd_available.clear();
    xvmc_osd_lock.unlock();
#endif // USING_XVMC

    vbuffers.DeleteBuffers();

    if (xv_color_conv_buf)
    {
        delete [] xv_color_conv_buf;
        xv_color_conv_buf = NULL;
    }

    if (delete_pause_frame)
    {
        if (av_pause_frame.buf)
        {
            delete [] av_pause_frame.buf;
            av_pause_frame.buf = NULL;
        }
        if (av_pause_frame.qscale_table)
        {
            delete [] av_pause_frame.qscale_table;
            av_pause_frame.qscale_table = NULL;
        }
    }

    for (uint i=0; i<XJ_shm_infos.size(); ++i)
    {
        X11S(XShmDetach(XJ_disp, &(XJ_shm_infos[i])));
        XvImage *image = (XvImage*) 
            xv_buffers[(unsigned char*)XJ_shm_infos[i].shmaddr];
        if (image)
        {
            if ((XImage*)image == (XImage*)XJ_non_xv_image)
                X11S(XDestroyImage((XImage*)XJ_non_xv_image));
            else
                X11S(XFree(image));
        }
        if (XJ_shm_infos[i].shmaddr)
            shmdt(XJ_shm_infos[i].shmaddr);
        if (XJ_shm_infos[i].shmid > 0)
            shmctl(XJ_shm_infos[0].shmid, IPC_RMID, 0);
    }
    XJ_shm_infos.clear();
    xv_buffers.clear();
    XJ_non_xv_image = NULL;

#ifdef USING_XVMC
    DeleteXvMCContext(XJ_disp, xvmc_ctx);
#endif // USING_XVMC
}

void VideoOutputXv::EmbedInWidget(WId wid, int x, int y, int w, int h)
{
    QMutexLocker locker(&global_lock);

    if (embedding)
    {
        MoveResize();
        return;
    }

    XJ_curwin = wid;

    VideoOutput::EmbedInWidget(wid, x, y, w, h);

    // Switch to GUI size
    if (display_res)
        display_res->SwitchToGUI();
}

void VideoOutputXv::StopEmbedding(void)
{
    if (!embedding)
        return;

    QMutexLocker locker(&global_lock);

    XJ_curwin = XJ_win;
    VideoOutput::StopEmbedding();

    // Switch back to resolution for full screen video
    if (display_res)
        display_res->SwitchToVideo(XJ_width, XJ_height);
}

VideoFrame *VideoOutputXv::GetNextFreeFrame(bool /*allow_unsafe*/)
{
    return vbuffers.GetNextFreeFrame(false, false);
}

/**
 * \fn VideoOutputXv::DiscardFrame(VideoFrame *frame)
 *  Frame is ready to be reused by decoder added to the
 *  done or available list.
 *
 * \param frame to discard.
 */
void VideoOutputXv::DiscardFrame(VideoFrame *frame)
{
    bool displaying = false;
    if (!frame)
        return;

#ifdef USING_XVMC
    vbuffers.LockFrame(frame, "DiscardFrame -- XvMC display check");
    if (frame && VideoOutputSubType() >= XVideoMC)
    {
        // Check display status
        VideoFrame* pframe = NULL;
        VideoFrame* osdframe = vbuffers.GetOSDFrame(frame);
        if (osdframe)
            vbuffers.SetOSDFrame(frame, NULL);
        else
            pframe = vbuffers.GetOSDParent(frame);

        SyncSurface(frame);
        displaying = IsDisplaying(frame);
        vbuffers.UnlockFrame(frame, "DiscardFrame -- XvMC display check A");

        SyncSurface(osdframe);
        displaying |= IsDisplaying(osdframe);

        if (!displaying && pframe)
            vbuffers.SetOSDFrame(frame, NULL);
    }
    else
        vbuffers.UnlockFrame(frame, "DiscardFrame -- XvMC display check B");
#endif

    if (displaying || vbuffers.HasChildren(frame))
        vbuffers.safeEnqueue(kVideoBuffer_displayed, frame);
    else
    {
        vbuffers.LockFrame(frame,   "DiscardFrame -- XvMC not displaying");
#ifdef USING_XVMC
        if (frame && VideoOutputSubType() >= XVideoMC)
        {
            GetRender(frame)->p_past_surface   = NULL;
            GetRender(frame)->p_future_surface = NULL;
        }
#endif
        vbuffers.UnlockFrame(frame, "DiscardFrame -- XvMC not displaying");
        vbuffers.RemoveInheritence(frame);
        vbuffers.DiscardFrame(frame);
    }
}

void VideoOutputXv::ClearAfterSeek(void)
{
    VERBOSE(VB_PLAYBACK, LOC + "ClearAfterSeek()");
    DiscardFrames(false);
#ifdef USING_XVMC
    if (VideoOutputSubType() > XVideo)
    {
        for (uint i=0; i<xvmc_surfs.size(); i++)
        {
            xvmc_vo_surf_t *surf = (xvmc_vo_surf_t*) xvmc_surfs[i];
            X11S(XvMCHideSurface(XJ_disp, &(surf->surface)));
        }
        DiscardFrames(true);
    }
#endif
}

#define DQ_COPY(DST, SRC) \
    do { \
        DST.insert(DST.end(), vbuffers.begin_lock(SRC), vbuffers.end(SRC)); \
        vbuffers.end_lock(); \
    } while (0)

void VideoOutputXv::DiscardFrames(bool next_frame_keyframe)
{ 
    if (VideoOutputSubType() <= XVideo)
    {
        vbuffers.DiscardFrames(next_frame_keyframe);
        return;
    }

#ifdef USING_XVMC
    frame_queue_t::iterator it;
    frame_queue_t syncs;
    frame_queue_t ula;
    frame_queue_t discards;

    {
        vbuffers.begin_lock(kVideoBuffer_displayed); // Lock X
        VERBOSE(VB_PLAYBACK, LOC + QString("DiscardFrames() 1: %1")
                .arg(vbuffers.GetStatus()));
        vbuffers.end_lock(); // Lock X
    }

    CheckDisplayedFramesForAvailability();

    {
        vbuffers.begin_lock(kVideoBuffer_displayed); // Lock Y

        DQ_COPY(syncs, kVideoBuffer_displayed);
        DQ_COPY(syncs, kVideoBuffer_pause);
        for (it = syncs.begin(); it != syncs.end(); ++it)
        {
            SyncSurface(*it, -1); // sync past
            SyncSurface(*it, +1); // sync future
            SyncSurface(*it,  0); // sync current
            //GetRender(*it)->p_past_surface   = NULL;
            //GetRender(*it)->p_future_surface = NULL;
        }
        VERBOSE(VB_PLAYBACK, LOC + QString("DiscardFrames() 2: %1")
                .arg(vbuffers.GetStatus()));
#if 0
        // Remove inheritence of all frames not in displayed or pause
        DQ_COPY(ula, kVideoBuffer_used);
        DQ_COPY(ula, kVideoBuffer_limbo);
        DQ_COPY(ula, kVideoBuffer_avail);
        
        for (it = ula.begin(); it != ula.end(); ++it)
            vbuffers.RemoveInheritence(*it);
#endif

        VERBOSE(VB_PLAYBACK, LOC + QString("DiscardFrames() 3: %1")
                .arg(vbuffers.GetStatus()));
        // create discard frame list
        DQ_COPY(discards, kVideoBuffer_used);
        DQ_COPY(discards, kVideoBuffer_limbo);

        vbuffers.end_lock(); // Lock Y
    }

    for (it = discards.begin(); it != discards.end(); ++it)
        DiscardFrame(*it);

    {
        vbuffers.begin_lock(kVideoBuffer_displayed); // Lock Z

        syncs.clear();
        DQ_COPY(syncs, kVideoBuffer_displayed);
        DQ_COPY(syncs, kVideoBuffer_pause);
        for (it = syncs.begin(); it != syncs.end(); ++it)
        {
            SyncSurface(*it, -1); // sync past
            SyncSurface(*it, +1); // sync future
            SyncSurface(*it,  0); // sync current
            //GetRender(*it)->p_past_surface   = NULL;
            //GetRender(*it)->p_future_surface = NULL;
        }

        VERBOSE(VB_PLAYBACK, LOC +
                QString("DiscardFrames() 4: %1 -- done() ")
                .arg(vbuffers.GetStatus()));
        
        vbuffers.end_lock(); // Lock Z
    }
#endif // USING_XVMC
}

#undef DQ_COPY

/** 
 * \fn VideoOutputXv::DoneDisplayingFrame(void)
 *  This is used to tell this class that the NPV will not
 *  call Show() on this frame again.
 *
 *  If the frame is not referenced elsewhere or all
 *  frames referencing it are done rendering this
 *  removes last displayed frame from used queue
 *  and adds it to the available list. If the frame is 
 *  still being used then it adds it to a special
 *  done displaying list that is checked when
 *  more frames are needed than in the available
 *  list.
 *
 */
void VideoOutputXv::DoneDisplayingFrame(void)
{
    if (VideoOutputSubType() <= XVideo)
    {
        vbuffers.DoneDisplayingFrame();
        return;
    }
#ifdef USING_XVMC
    if (vbuffers.size(kVideoBuffer_used))
    {
        VideoFrame *frame = vbuffers.head(kVideoBuffer_used);
        DiscardFrame(frame);
        VideoFrame *osdframe = vbuffers.GetOSDFrame(frame);
        if (osdframe)
            DiscardFrame(osdframe);
    }
    CheckDisplayedFramesForAvailability();
#endif
}

/**
 * \fn VideoOutputXv::PrepareFrameXvMC(VideoFrame *frame)
 *  
 *  
 */
void VideoOutputXv::PrepareFrameXvMC(VideoFrame *frame)
{
    (void)frame;
#ifdef USING_XVMC
    xvmc_render_state_t *render = NULL, *osdrender = NULL;
    VideoFrame *osdframe = NULL;

    if (frame)
    {
        global_lock.lock();
        framesPlayed = frame->frameNumber + 1;
        global_lock.unlock();

        vbuffers.LockFrame(frame, "PrepareFrameXvMC");
        SyncSurface(frame);
        render = GetRender(frame);
        render->state |= MP_XVMC_STATE_DISPLAY_PENDING;
        osdframe = vbuffers.GetOSDFrame(frame);
        vbuffers.UnlockFrame(frame, "PrepareFrameXvMC");
    }

    if (osdframe)
    {
        vbuffers.LockFrame(osdframe, "PrepareFrameXvMC -- osd");
        SyncSurface(osdframe);
        osdrender = GetRender(osdframe);
        osdrender->state |= MP_XVMC_STATE_DISPLAY_PENDING;
        vbuffers.UnlockFrame(osdframe, "PrepareFrameXvMC -- osd");
    }
#endif // USING_XVMC
}

/**
 * \fn VideoOutputXv::PrepareFrameXv(VideoFrame *frame)
 *  
 *  
 */
void VideoOutputXv::PrepareFrameXv(VideoFrame *frame)
{
    if (!frame)
        frame = vbuffers.GetScratchFrame();

    XvImage *image = NULL;
    {
        QMutexLocker locker(&global_lock);
        vbuffers.LockFrame(frame, "PrepareFrameXv");
        framesPlayed = frame->frameNumber + 1;
        image        = (XvImage*) xv_buffers[frame->buf];
        vbuffers.UnlockFrame(frame, "PrepareFrameXv");
    }

    if (image && (GUID_YV12_PLANAR == xv_chroma))
    {
        vbuffers.LockFrame(frame, "PrepareFrameXv -- color conversion");
        int width = frame->width;
        int height = frame->height;

        memcpy(xv_color_conv_buf, (unsigned char *)image->data + 
               (width * height), width * height / 4);
        memcpy((unsigned char *)image->data + (width * height),
               (unsigned char *)image->data + (width * height) * 5 / 4,
               width * height / 4);
        memcpy((unsigned char *)image->data + (width * height) * 5 / 4,
               xv_color_conv_buf, width * height / 4);
        vbuffers.UnlockFrame(frame, "PrepareFrameXv -- color conversion");
    }

    if (vbuffers.GetScratchFrame() == frame)
        vbuffers.SetLastShownFrameToScratch();
}

/**
 * \fn VideoOutputXv::PrepareFrameMem(VideoFrame*, FrameScanType)
 *  
 *  
 */
void VideoOutputXv::PrepareFrameMem(VideoFrame *buffer, FrameScanType /*scan*/)
{
    if (!buffer)
        buffer = vbuffers.GetScratchFrame();

    vbuffers.LockFrame(buffer, "PrepareFrameMem");

    framesPlayed = buffer->frameNumber + 1;
    int width = buffer->width;
    int height = buffer->height;

    vbuffers.UnlockFrame(buffer, "PrepareFrameMem");

    // bad way to throttle frame display for non-Xv mode.
    // calculate fps we can do and skip enough frames so we don't exceed.
    if (non_xv_frames_shown == 0)
        non_xv_stop_time = time(NULL) + 4;

    if ((!non_xv_fps) && (time(NULL) > non_xv_stop_time))
    {
        non_xv_fps = (int)(non_xv_frames_shown / 4);

        if (non_xv_fps < 25)
        {
            non_xv_show_frame = 120 / non_xv_frames_shown + 1;
            VERBOSE(VB_IMPORTANT, LOC_ERR + "\n"
                    "***\n"
                    "* Your system is not capable of displaying the\n"
                    "* full framerate at "
                    <<dispw<<"x"<<disph<<" resolution.  Frames\n"
                    "* will be skipped in order to keep the audio and\n"
                    "* video in sync.\n");
        }
    }

    non_xv_frames_shown++;

    if ((non_xv_show_frame != 1) && (non_xv_frames_shown % non_xv_show_frame))
        return;

    if (!XJ_non_xv_image)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "XJ_non_xv_image == NULL");
        return;
    }

    unsigned char *sbuf = new unsigned char[dispw * disph * 3 / 2];
    AVPicture image_in, image_out;
    ImgReSampleContext *scontext;

    avpicture_fill(&image_out, (uint8_t *)sbuf, PIX_FMT_YUV420P,
                   dispw, disph);

    vbuffers.LockFrame(buffer, "PrepareFrameMem");
    if ((dispw == width) && (disph == height))
    {
        memcpy(sbuf, buffer->buf, width * height * 3 / 2);
    }
    else
    {
        avpicture_fill(&image_in, buffer->buf, PIX_FMT_YUV420P,
                       width, height);
        scontext = img_resample_init(dispw, disph, width, height);
        img_resample(scontext, &image_out, &image_in);

        img_resample_close(scontext);
    }
    vbuffers.UnlockFrame(buffer, "PrepareFrameMem");

    avpicture_fill(&image_in, (uint8_t *)XJ_non_xv_image->data, 
                   non_xv_av_format, dispw, disph);

    img_convert(&image_in, non_xv_av_format, &image_out, PIX_FMT_YUV420P,
                dispw, disph);

    {
        QMutexLocker locker(&global_lock);
        X11L;
        if (XShm == video_output_subtype)
            XShmPutImage(XJ_disp, XJ_curwin, XJ_gc, XJ_non_xv_image,
                         0, 0, 0, 0, dispw, disph, False);
        else
            XPutImage(XJ_disp, XJ_curwin, XJ_gc, XJ_non_xv_image, 
                      0, 0, 0, 0, dispw, disph);
        X11U;
    }

    delete [] sbuf;
}

// this is documented in videooutbase.cpp
void VideoOutputXv::PrepareFrame(VideoFrame *buffer, FrameScanType scan)
{
    if (IsErrored())
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "IsErrored() in PrepareFrame()");
        return;
    }

    if (VideoOutputSubType() > XVideo)
        PrepareFrameXvMC(buffer);
    else if (VideoOutputSubType() == XVideo)
        PrepareFrameXv(buffer);
    else
        PrepareFrameMem(buffer, scan);
}

static void calc_bob(FrameScanType scan, int imgh, int disphoff,
                     int imgy, int dispyoff,
                     int frame_height, int top_field_first,
                     int &field, int &src_y, int &dest_y,
                     int& xv_src_y_incr, int &xv_dest_y_incr)
{
    int dst_half_line_in_src = 0, dest_y_incr = 0, src_y_incr = 0;
    field = 3;
    src_y = imgy;
    dest_y = dispyoff;
    xv_src_y_incr = 0;
    // a negative offset y gives us bobbing, so adjust...
    if (dispyoff < 0)
    {
        dest_y_incr = -dispyoff;
        src_y_incr = (int) (dest_y_incr * imgh / disphoff);
        xv_src_y_incr -= (int) (0.5 * dest_y_incr * imgh / disphoff);
    }

    if ((scan == kScan_Interlaced && top_field_first == 1) ||
        (scan == kScan_Intr2ndField && top_field_first == 0))
    {
        field = 1;
        xv_src_y_incr += - imgy / 2;
    }
    else if ((scan == kScan_Interlaced && top_field_first == 0) ||
             (scan == kScan_Intr2ndField && top_field_first == 1))
    {
        field = 2;
        xv_src_y_incr += (frame_height - imgy) / 2;

        dst_half_line_in_src =
            max((int) round((((double)disphoff)/imgh) - 0.00001), 0);
    }
    src_y += src_y_incr;
    dest_y += dest_y_incr;

#define NVIDIA_6629
#ifdef NVIDIA_6629
    xv_dest_y_incr = dst_half_line_in_src;
    // nVidia v 66.29, does proper compensation when imgh==frame_height
    // but we need to compensate when the difference is >= 5%
    int mod = 0;
    if (frame_height>=(int)(imgh+(0.05*frame_height)) && 2==field)
    {
        //int nrml = (int) round((((double)disphoff)/frame_height) - 0.00001);
        mod = -dst_half_line_in_src;
        dest_y += mod;
        xv_dest_y_incr -= mod;
    }
#else
    dest_y += dst_half_line_in_src;
#endif

    // DEBUG
#if 0
    static int last_dest_y_field[3] = { -1000, -1000, -1000, };
    int last_dest_y = last_dest_y_field[field];

    if (last_dest_y != dest_y)
    {
        cerr<<"####### Field "<<field<<" #######"<<endl;
        cerr<<"         src_y: "<<src_y<<endl;
        cerr<<"        dest_y: "<<dest_y<<endl;
        cerr<<" xv_src_y_incr: "<<xv_src_y_incr<<endl;
        cerr<<"xv_dest_y_incr: "<<xv_dest_y_incr<<endl;
        cerr<<"      disphoff: "<<disphoff<<endl;
        cerr<<"          imgh: "<<imgh<<endl;
        cerr<<"           mod: "<<mod<<endl;
        cerr<<endl;
    }
    last_dest_y_field[field] = dest_y;
#endif
}

void VideoOutputXv::ShowXvMC(FrameScanType scan)
{
    (void)scan;
#ifdef USING_XVMC
    VideoFrame *frame = NULL;
    bool using_pause_frame = false;

    vbuffers.begin_lock(kVideoBuffer_pause);
    if (vbuffers.size(kVideoBuffer_pause))
    {
        frame = vbuffers.head(kVideoBuffer_pause);
#ifdef DEBUG_PAUSE
        VERBOSE(VB_PLAYBACK, LOC + QString("use pause frame: %1 ShowXvMC")
                .arg(DebugString(frame)));
#endif // DEBUG_PAUSE
        using_pause_frame = true;
    }
    else if (vbuffers.size(kVideoBuffer_used))
        frame = vbuffers.head(kVideoBuffer_used);
    vbuffers.end_lock();

    if (!frame)
    {
        VERBOSE(VB_PLAYBACK, LOC + "ShowXvMC(): No frame to show");
        return;
    }

    vbuffers.LockFrame(frame, "ShowXvMC");

    // calculate bobbing params
    int field = 3, src_y = imgy, dest_y = dispyoff;
    int xv_src_y_incr = 0, xv_dest_y_incr = 0;
    if (m_deinterlacing)
    {
        calc_bob(scan, imgh, disphoff, imgy, dispyoff,
                 frame->height, frame->top_field_first,
                 field, src_y, dest_y, xv_src_y_incr, xv_dest_y_incr);
    }
    if (hasVLDAcceleration())
    {   // don't do bob-adjustment for VLD drivers
        src_y = imgy;
        dest_y = dispyoff;
    }

    // get and try to lock OSD frame, if it exists
    VideoFrame *osdframe = vbuffers.GetOSDFrame(frame);
    if (osdframe && !vbuffers.TryLockFrame(osdframe, "ShowXvMC -- osd"))
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "ShowXvMC(): Unable to get OSD lock");
        vbuffers.safeEnqueue(kVideoBuffer_displayed, osdframe);
        osdframe = NULL;
    }

    // set showing surface, depending on existance of osd
    xvmc_render_state_t *showingsurface = (osdframe) ?
        GetRender(osdframe) : GetRender(frame);
    XvMCSurface *surf = showingsurface->p_surface;

    // actually display the frame 
    X11L;
    XvMCPutSurface(XJ_disp, surf, XJ_curwin,
                   imgx, src_y, imgw, imgh,
                   dispxoff, dest_y, dispwoff, disphoff, field);
    XFlush(XJ_disp); // send XvMCPutSurface call to X11 server
    X11U;

    // if not using_pause_frame, clear old process buffer
    if (!using_pause_frame)
    {
        while (vbuffers.size(kVideoBuffer_pause))
            DiscardFrame(vbuffers.dequeue(kVideoBuffer_pause));
    }
    // clear any displayed frames not on screen
    CheckDisplayedFramesForAvailability();

    // unlock the frame[s]
    vbuffers.UnlockFrame(osdframe, "ShowXvMC -- OSD");
    vbuffers.UnlockFrame(frame, "ShowXvMC");

    // make sure osdframe is eventually added to available
    vbuffers.safeEnqueue(kVideoBuffer_displayed, osdframe);
#endif // USING_XVMC
}

void VideoOutputXv::ShowXVideo(FrameScanType scan)
{
    VideoFrame *frame = GetLastShownFrame();

    vbuffers.LockFrame(frame, "ShowXVideo");

    XvImage *image = (XvImage*) xv_buffers[frame->buf];
    if (!image)
    {
        vbuffers.UnlockFrame(frame, "ShowXVideo");
        return;
    }

    int field = 3, src_y = imgy, dest_y = dispyoff, xv_src_y_incr = 0, xv_dest_y_incr = 0;
    if (m_deinterlacing && (m_deintfiltername == "bobdeint"))
    {
        calc_bob(scan, imgh, disphoff, imgy, dispyoff,
                 frame->height, frame->top_field_first,
                 field, src_y, dest_y, xv_src_y_incr, xv_dest_y_incr);
        src_y += xv_src_y_incr;
        dest_y += xv_dest_y_incr;
    }

    vbuffers.UnlockFrame(frame, "ShowXVideo");
    {
        QMutexLocker locker(&global_lock);
        vbuffers.LockFrame(frame, "ShowXVideo");
        X11S(XvShmPutImage(XJ_disp, xv_port, XJ_curwin,
                           XJ_gc, image, imgx, src_y, imgw,
                           (3 != field) ? (imgh/2) : imgh,
                           dispxoff, dest_y, dispwoff, disphoff, False));
        vbuffers.UnlockFrame(frame, "ShowXVideo");
    }
}

// this is documented in videooutbase.cpp
void VideoOutputXv::Show(FrameScanType scan)
{
    if (IsErrored())
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "IsErrored() is true in Show()");
        return;
    }

    if (needrepaint && (VideoOutputSubType() >= XVideo))
        DrawUnusedRects(/* don't do a sync*/false);

    if (VideoOutputSubType() > XVideo)
        ShowXvMC(scan);
    else if (VideoOutputSubType() == XVideo)
        ShowXVideo(scan);

    X11S(XSync(XJ_disp, False));
}

void VideoOutputXv::DrawUnusedRects(bool sync)
{
    // boboff assumes the smallest interlaced resolution is 480 lines - 5%
    int boboff = (int)round(((double)disphoff) / 456 - 0.00001);
    boboff = (m_deinterlacing && m_deintfiltername == "bobdeint") ? boboff : 0;

    if (chroma_osd && chroma_osd->GetImage() && needrepaint)
    {
        X11L;
        XShmPutImage(XJ_disp, XJ_curwin, XJ_gc, chroma_osd->GetImage(),
                     0, 0, 0, 0, dispw, disph, False);
        if (sync)
            XSync(XJ_disp, false);
        X11U;

        needrepaint = false;
        return;
    }

    X11L;

    if (xv_draw_colorkey && needrepaint)
    {
        XSetForeground(XJ_disp, XJ_gc, xv_colorkey);
        XFillRectangle(XJ_disp, XJ_curwin, XJ_gc, dispx, 
                       dispy + boboff, dispw, disph - 2 * boboff);
        needrepaint = false;
    }

    // Draw black in masked areas
    XSetForeground(XJ_disp, XJ_gc, XJ_black);

    if (dispxoff > dispx) // left
        XFillRectangle(XJ_disp, XJ_curwin, XJ_gc, 
                       dispx, dispy, dispxoff - dispx, disph);
    if (dispxoff + dispwoff < dispx + dispw) // right
        XFillRectangle(XJ_disp, XJ_curwin, XJ_gc, 
                       dispxoff + dispwoff, dispy, 
                       (dispx + dispw) - (dispxoff + dispwoff), disph);
    if (dispyoff + boboff > dispy) // top of screen
        XFillRectangle(XJ_disp, XJ_curwin, XJ_gc, 
                       dispx, dispy, dispw, dispyoff + boboff - dispy);
    if (dispyoff + disphoff < dispy + disph) // bottom of screen
        XFillRectangle(XJ_disp, XJ_curwin, XJ_gc, 
                       dispx, dispyoff + disphoff, 
                       dispw, (dispy + disph) - (dispyoff + disphoff));

    if (sync)
        XSync(XJ_disp, false);

    X11U;
}

/**
 * \fn VideoOutputXv::DrawSlice(VideoFrame *frame, int x, int y, int w, int h)
 *  
 *  
 */
void VideoOutputXv::DrawSlice(VideoFrame *frame, int x, int y, int w, int h)
{
    (void)frame;
    (void)x;
    (void)y;
    (void)w;
    (void)h;

    if (VideoOutputSubType() <= XVideo)
        return;

#ifdef USING_XVMC
    xvmc_render_state_t *render = GetRender(frame);
    // disable questionable ffmpeg surface munging
    if (render->p_past_surface == render->p_surface)
        render->p_past_surface = NULL;
    vbuffers.AddInheritence(frame);

    Status status;
    if (hasVLDAcceleration())
    {
        vbuffers.LockFrame(frame, "DrawSlice -- VLD");
        X11S(status = XvMCPutSlice2(XJ_disp, xvmc_ctx, 
                                    (char*)render->slice_data, 
                                    render->slice_datalen, 
                                    render->slice_code));
        if (Success != status)
            VERBOSE(VB_PLAYBACK, LOC_ERR + "XvMCPutSlice: "<<status);

#if 0
        // TODO are these three lines really needed???
        render->start_mv_blocks_num = 0;
        render->filled_mv_blocks_num = 0;
        render->next_free_data_block_num = 0;
#endif

        vbuffers.UnlockFrame(frame, "DrawSlice -- VLD");
    }
    else
    {
        vector<const VideoFrame*> locks;
        locks.push_back(vbuffers.PastFrame(frame));
        locks.push_back(vbuffers.FutureFrame(frame));
        locks.push_back(frame);
        vbuffers.LockFrames(locks, "DrawSlice");

        // Sync past & future I and P frames
        X11S(status =
             XvMCRenderSurface(XJ_disp, xvmc_ctx, 
                               render->picture_structure, 
                               render->p_surface,
                               render->p_past_surface, 
                               render->p_future_surface,
                               render->flags,
                               render->filled_mv_blocks_num,
                               render->start_mv_blocks_num,
                               (XvMCMacroBlockArray *)frame->priv[1], 
                               (XvMCBlockArray *)frame->priv[0]));

        if (Success != status)
            VERBOSE(VB_PLAYBACK, LOC_ERR +
                    QString("XvMCRenderSurface: %1 (%2)")
                    .arg(ErrorStringXvMC(status)).arg(status));
        else
            FlushSurface(frame);

        render->start_mv_blocks_num = 0;
        render->filled_mv_blocks_num = 0;
        render->next_free_data_block_num = 0;
        vbuffers.UnlockFrames(locks, "DrawSlice");
    }
#endif // USING_XVMC
}

// documented in videooutbase.cpp
void VideoOutputXv::VideoAspectRatioChanged(float aspect)
{
    QMutexLocker locker(&global_lock);
    VideoOutput::VideoAspectRatioChanged(aspect);
}

void VideoOutputXv::UpdatePauseFrame(void)
{
    if (VideoOutputSubType() <= XVideo)
    {
        // Try used frame first, then fall back to scratch frame.
        vbuffers.LockFrame(&av_pause_frame, "UpdatePauseFrame -- pause");

        vbuffers.begin_lock(kVideoBuffer_used);
        VideoFrame *used_frame = NULL;
        if (vbuffers.size(kVideoBuffer_used) > 0)
        {
            used_frame = vbuffers.head(kVideoBuffer_used);
            if (!vbuffers.TryLockFrame(used_frame, "UpdatePauseFrame -- used"))
                used_frame = NULL;
        }
        if (used_frame)
        {
            CopyFrame(&av_pause_frame, used_frame);
            vbuffers.UnlockFrame(used_frame, "UpdatePauseFrame -- used");
        }
        vbuffers.end_lock();

        if (!used_frame &&
            vbuffers.TryLockFrame(vbuffers.GetScratchFrame(),
                                  "UpdatePauseFrame -- scratch"))
        {
            vbuffers.GetScratchFrame()->frameNumber = framesPlayed - 1;
            CopyFrame(&av_pause_frame, vbuffers.GetScratchFrame());
            vbuffers.UnlockFrame(vbuffers.GetScratchFrame(),
                                 "UpdatePauseFrame -- scratch");
        }
        vbuffers.UnlockFrame(&av_pause_frame, "UpdatePauseFrame - used");
    }
#ifdef USING_XVMC
    else
    {
        if (vbuffers.size(kVideoBuffer_pause)>1)
        {
            VERBOSE(VB_PLAYBACK, LOC_ERR + "UpdatePauseFrame(): "
                    "Pause buffer size>1 check, " + QString("size = %1")
                    .arg(vbuffers.size(kVideoBuffer_pause)));
            while (vbuffers.size(kVideoBuffer_pause))
                DiscardFrame(vbuffers.dequeue(kVideoBuffer_pause));
            CheckDisplayedFramesForAvailability();
        } else if (1 == vbuffers.size(kVideoBuffer_pause))
        {
            VideoFrame *frame = vbuffers.dequeue(kVideoBuffer_used);
            if (frame)
            {
                while (vbuffers.size(kVideoBuffer_pause))
                    DiscardFrame(vbuffers.dequeue(kVideoBuffer_pause));
                vbuffers.safeEnqueue(kVideoBuffer_pause, frame);
                VERBOSE(VB_PLAYBACK, LOC + "UpdatePauseFrame(): "
                        "XvMC using NEW pause frame");
            }
            else
                VERBOSE(VB_PLAYBACK, LOC + "UpdatePauseFrame(): "
                        "XvMC using OLD pause frame");
            return;
        }

        frame_queue_t::iterator it =
            vbuffers.begin_lock(kVideoBuffer_displayed);

        VERBOSE(VB_PLAYBACK, LOC + "UpdatePauseFrame -- XvMC");
        if (vbuffers.size(kVideoBuffer_displayed))
        {
            VERBOSE(VB_PLAYBACK, LOC + "UpdatePauseFrame -- XvMC: "
                    "\n\t\t\tFound a pause frame in display");

            VideoFrame *frame = vbuffers.tail(kVideoBuffer_displayed);
            if (vbuffers.GetOSDParent(frame))
                frame = vbuffers.GetOSDParent(frame);
            vbuffers.safeEnqueue(kVideoBuffer_pause, frame);
        }
        vbuffers.end_lock();

        if (1 != vbuffers.size(kVideoBuffer_pause))
        {
            VERBOSE(VB_PLAYBACK, LOC + "UpdatePauseFrame -- XvMC: "
                    "\n\t\t\tDid NOT find a pause frame");
        }
    }
#endif
}

void VideoOutputXv::ProcessFrameXvMC(VideoFrame *frame, OSD *osd)
{
    (void)frame;
    (void)osd;
#ifdef USING_XVMC
    if (frame)
    {
        vbuffers.LockFrame(frame, "ProcessFrameXvMC");
        while (vbuffers.size(kVideoBuffer_pause))
            DiscardFrame(vbuffers.dequeue(kVideoBuffer_pause));
    }
    else
    {
        bool success = false;
        
        frame_queue_t::iterator it = vbuffers.begin_lock(kVideoBuffer_pause);
        if (vbuffers.size(kVideoBuffer_pause))
        {
            frame = vbuffers.head(kVideoBuffer_pause);
            success = vbuffers.TryLockFrame(
                frame, "ProcessFrameXvMC -- reuse");
        }
        vbuffers.end_lock();

        if (success)
        {
#ifdef DEBUG_PAUSE
            VERBOSE(VB_PLAYBACK, LOC + "ProcessFrameXvMC: " +
                    QString("Use pause frame: %1").arg(DebugString(frame)));
#endif // DEBUG_PAUSE
            vbuffers.SetOSDFrame(frame, NULL);
        }
        else
        {
            VERBOSE(VB_IMPORTANT, LOC + "ProcessFrameXvMC: "
                    "Tried to reuse frame but failed");
            frame = NULL;
        }
    }

    if (!frame)
    {
        VERBOSE(VB_IMPORTANT, LOC + "ProcessFrameXvMC: "
                "Called without frame");
        return;
    }

    if (chroma_osd)
    {
        QMutexLocker locker(&global_lock);
        needrepaint |= chroma_osd->ProcessOSD(osd);
        vbuffers.UnlockFrame(frame, "ProcessFrameXvMC");
        return;
    }

    if (!xvmc_buf_attr->GetOSDNum())
    {
        vbuffers.UnlockFrame(frame, "ProcessFrameXvMC");
        return;
    }

    VideoFrame * old_osdframe = vbuffers.GetOSDFrame(frame);
    if (old_osdframe)
    {
        VERBOSE(VB_IMPORTANT, LOC + "ProcessFrameXvMC:\n\t\t\t" +
                QString("Warning, %1 is still marked as the OSD frame of %2.")
                .arg(DebugString(old_osdframe, true))
                .arg(DebugString(frame, true)));

        vbuffers.SetOSDFrame(frame, NULL);
    }

    XvMCOSD* xvmc_osd = NULL;
    if (!embedding && osd)
        xvmc_osd = GetAvailableOSD();

    if (xvmc_osd && xvmc_osd->IsValid())
    {
        VideoFrame *osdframe = NULL;
        int ret = DisplayOSD(xvmc_osd->OSDFrame(), osd, -1,
                             xvmc_osd->GetRevision());
        OSDSurface *osdsurf = osd->Display();
        if (osdsurf)
            xvmc_osd->SetRevision(osdsurf->GetRevision());
        if (ret >= 0 && xvmc_osd->NeedFrame())
        {
            // If there are no available buffer, try to toss old
            // displayed frames.
            if (!vbuffers.size(kVideoBuffer_avail))
                CheckDisplayedFramesForAvailability();

            // If tossing doesn't work try hiding showing frames,
            // then tossing displayed frames.
            if (!vbuffers.size(kVideoBuffer_avail))
            {
                frame_queue_t::iterator it;
                it = vbuffers.begin_lock(kVideoBuffer_displayed);
                for (;it != vbuffers.end(kVideoBuffer_displayed); ++it)
                    if (*it != frame)
                        X11S(XvMCHideSurface(XJ_disp,
                                             GetRender(*it)->p_surface));
                vbuffers.end_lock();

                CheckDisplayedFramesForAvailability();
            }

            // If there is an available buffer grab it.
            if (vbuffers.size(kVideoBuffer_avail))
            {
                osdframe = vbuffers.GetNextFreeFrame(false, false);
                // Check for error condition..
                if (frame == osdframe)
                {
                    VERBOSE(VB_IMPORTANT, LOC_ERR +
                            QString("ProcessFrameXvMC: %1 %2")
                            .arg(DebugString(frame, true))
                            .arg(vbuffers.GetStatus()));
                    osdframe = NULL;
                }
            }

            if (osdframe && vbuffers.TryLockFrame(
                    osdframe, "ProcessFrameXvMC -- OSD"))
            {
                vbuffers.SetOSDFrame(osdframe, NULL);
                xvmc_osd->CompositeOSD(frame, osdframe);
                vbuffers.UnlockFrame(osdframe, "ProcessFrameXvMC -- OSD");
                vbuffers.SetOSDFrame(frame, osdframe);
            }
            else
            {
                VERBOSE(VB_IMPORTANT, LOC_ERR + "ProcessFrameXvMC: "
                        "Failed to get OSD lock");
                DiscardFrame(osdframe);
            }
        }
        if (ret >= 0 && !xvmc_osd->NeedFrame())
        {
            xvmc_osd->CompositeOSD(frame);
        }
    }
    if (xvmc_osd)
        ReturnAvailableOSD(xvmc_osd);
    vbuffers.UnlockFrame(frame, "ProcessFrameXvMC");            
#endif // USING_XVMC
}

#ifdef USING_XVMC
XvMCOSD* VideoOutputXv::GetAvailableOSD()
{
    if (xvmc_buf_attr->GetOSDNum() > 1)
    {
        XvMCOSD *val = NULL;
        xvmc_osd_lock.lock();
        while (!xvmc_osd_available.size())
        {
            xvmc_osd_lock.unlock();
            usleep(50);
            xvmc_osd_lock.lock();
        }
        val = xvmc_osd_available.dequeue();
        xvmc_osd_lock.unlock();
        return val;
    }
    else if (xvmc_buf_attr->GetOSDNum() > 0)
    {
        xvmc_osd_lock.lock();
        return xvmc_osd_available.head();
    }
    return NULL;
}
#endif // USING_XVMC

#ifdef USING_XVMC
void VideoOutputXv::ReturnAvailableOSD(XvMCOSD *avail)
{
    if (xvmc_buf_attr->GetOSDNum() > 1)
    {
        xvmc_osd_lock.lock();
        xvmc_osd_available.push_front(avail);
        xvmc_osd_lock.unlock();
    }
    else if (xvmc_buf_attr->GetOSDNum() > 0)
    {
        xvmc_osd_lock.unlock();
    }
}
#endif // USING_XVMC

void VideoOutputXv::ProcessFrameMem(VideoFrame *frame, OSD *osd,
                                    FilterChain *filterList,
                                    NuppelVideoPlayer *pipPlayer)
{
    bool deint_proc = m_deinterlacing && (m_deintFilter != NULL);
    bool pauseframe = false;
    if (!frame)
    {
        frame = vbuffers.GetScratchFrame();
        vector<const VideoFrame*> locks;
        locks.push_back(frame);
        locks.push_back(&av_pause_frame);
        vbuffers.LockFrames(locks, "ProcessFrameMem -- pause");
        CopyFrame(frame, &av_pause_frame);
        vbuffers.UnlockFrames(locks, "ProcessFrameMem -- pause");
        pauseframe = true;
    }

    vbuffers.LockFrame(frame, "ProcessFrameMem");

    if (!pauseframe)
    {
        if (filterList)
            filterList->ProcessFrame(frame);
        
        if (deint_proc && m_deinterlaceBeforeOSD)
            m_deintFilter->ProcessFrame(frame);
    }

    ShowPip(frame, pipPlayer);

    if (osd && !embedding)
    {
        DisplayOSD(frame, osd);
    }

    if (!pauseframe && deint_proc && !m_deinterlaceBeforeOSD)
        m_deintFilter->ProcessFrame(frame);

    vbuffers.UnlockFrame(frame, "ProcessFrameMem");
}

// this is documented in videooutbase.cpp
void VideoOutputXv::ProcessFrame(VideoFrame *frame, OSD *osd,
                                 FilterChain *filterList,
                                 NuppelVideoPlayer *pipPlayer)
{
    if (IsErrored())
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "IsErrored() in ProcessFrame()");
        return;
    }

    if (VideoOutputSubType() <= XVideo)
        ProcessFrameMem(frame, osd, filterList, pipPlayer);
    else
        ProcessFrameXvMC(frame, osd);
}

int VideoOutputXv::ChangePictureAttribute(int attribute, int newValue)
{
    int value;
    int i, howmany, port_min, port_max, range;
    char *attrName = NULL;
    Atom attributeAtom;
    XvAttribute *attributes;

    switch (attribute)
    {
        case kPictureAttribute_Brightness:
            attrName = "XV_BRIGHTNESS";
            break;
        case kPictureAttribute_Contrast:
            attrName = "XV_CONTRAST";
            break;
        case kPictureAttribute_Colour:
            attrName = "XV_SATURATION";
            break;
        case kPictureAttribute_Hue:
            attrName = "XV_HUE";
            break;
    }

    if (!attrName)
        return -1;

    if (newValue < 0) newValue = 0;
    if (newValue >= 100) newValue = 99;

    X11S(attributeAtom = XInternAtom (XJ_disp, attrName, False));
    if (!attributeAtom) {
        return -1;
    }

    X11S(attributes = XvQueryPortAttributes(XJ_disp, xv_port, &howmany));
    if (!attributes) {
        return -1;
    }

    for (i = 0; i < howmany; i++) {
        if (!strcmp(attrName, attributes[i].name)) {
            port_min = attributes[i].min_value;
            port_max = attributes[i].max_value;
            range = port_max - port_min;

            value = (int) (port_min + (range/100.0) * newValue);

            X11S(XvSetPortAttribute(XJ_disp, xv_port, attributeAtom, value));

            return newValue;
        }
    }

    return -1;
}

void VideoOutputXv::CheckDisplayedFramesForAvailability(void)
{
#ifdef USING_XVMC
    frame_queue_t::iterator it;

    if (xvmc_buf_attr->IsAggressive())
    {
        it = vbuffers.begin_lock(kVideoBuffer_displayed);
        for (;it != vbuffers.end(kVideoBuffer_displayed); ++it)
        {
            VideoFrame* frame = *it;
            frame_queue_t c = vbuffers.Children(frame);
            frame_queue_t::iterator cit = c.begin();
            for (; cit != c.end(); ++cit)
            {
                VideoFrame *cframe = *cit;
                vbuffers.LockFrame(cframe, "CDFForAvailability 1");
                if (!IsRendering(cframe))
                {
                    GetRender(cframe)->p_past_surface   = NULL;
                    GetRender(cframe)->p_future_surface = NULL;
                    vbuffers.RemoveInheritence(cframe);
                    vbuffers.UnlockFrame(cframe, "CDFForAvailability 2");
                    if (!vbuffers.HasChildren(frame))
                        break;
                    else
                    {
                        c = vbuffers.Children(frame);
                        cit = c.begin();
                    }
                }
                else
                    vbuffers.UnlockFrame(cframe, "CDFForAvailability 3");
            }
        }
        vbuffers.end_lock();
    }

    it = vbuffers.begin_lock(kVideoBuffer_displayed);
    for (;it != vbuffers.end(kVideoBuffer_displayed); ++it)
        vbuffers.RemoveInheritence(*it);
    vbuffers.end_lock();

    it = vbuffers.begin_lock(kVideoBuffer_displayed);
    while (it != vbuffers.end(kVideoBuffer_displayed))
    {
        VideoFrame* pframe = *it;
        SyncSurface(pframe);
        if (!IsDisplaying(pframe))
        {
            frame_queue_t children = vbuffers.Children(pframe);
            if (!children.empty())
            {
#if 0
                VERBOSE(VB_PLAYBACK, LOC + QString(
                            "Frame %1 w/children: %2 is being held for later "
                            "discarding.")
                        .arg(DebugString(pframe, true))
                        .arg(DebugString(children)));
#endif
                frame_queue_t::iterator cit;
                for (cit = children.begin(); cit != children.end(); ++cit)
                {
                    if (vbuffers.contains(kVideoBuffer_avail, *cit))
                    {
                        VERBOSE(VB_IMPORTANT, LOC_ERR + QString(
                                    "Child     %1 was already marked "
                                    "as available.").arg(DebugString(*cit)));
                    }
                }
            }
            else
            {
                vbuffers.RemoveInheritence(pframe);
                vbuffers.safeEnqueue(kVideoBuffer_avail, pframe);
                vbuffers.end_lock();
                it = vbuffers.begin_lock(kVideoBuffer_displayed);
                continue;
            }
        }
        ++it;
    }
    vbuffers.end_lock();

#endif // USING_XVMC
}

bool VideoOutputXv::IsDisplaying(VideoFrame* frame)
{
    (void)frame;
#ifdef USING_XVMC
    xvmc_render_state_t *render = GetRender(frame);
    if (render)
    {
        Display *disp     = render->disp;
        XvMCSurface *surf = render->p_surface;
        int res = 0, status = 0;
        if (disp && surf)
            X11S(res = XvMCGetSurfaceStatus(disp, surf, &status));
        if (Success == res)
            return (status & XVMC_DISPLAYING);
        else
            VERBOSE(VB_PLAYBACK, LOC_ERR + "IsDisplaying(): " +
                    QString("XvMCGetSurfaceStatus %1").arg(res));
    }
#endif // USING_XVMC
    return false;
}

bool VideoOutputXv::IsRendering(VideoFrame* frame)
{
    (void)frame;
#ifdef USING_XVMC
    xvmc_render_state_t *render = GetRender(frame);
    if (render)
    {
        Display *disp     = render->disp;
        XvMCSurface *surf = render->p_surface;
        int res = 0, status = 0;
        if (disp && surf)
            X11S(res = XvMCGetSurfaceStatus(disp, surf, &status));
        if (Success == res)
            return (status & XVMC_RENDERING);
        else
            VERBOSE(VB_PLAYBACK, LOC_ERR + "IsRendering(): " +
                    QString("XvMCGetSurfaceStatus %1").arg(res));
    }
#endif // USING_XVMC
    return false;
}

void VideoOutputXv::SyncSurface(VideoFrame* frame, int past_future)
{
    (void)frame;
    (void)past_future;
#ifdef USING_XVMC
    xvmc_render_state_t *render = GetRender(frame);
    if (render)
    {
        Display *disp     = render->disp;
        XvMCSurface *surf = render->p_surface;
        if (past_future == -1)
            surf = render->p_past_surface;
        else if (past_future == +1)
            surf = render->p_future_surface;

        if (disp && surf)
        {
            int status = 0, res = Success;

            X11S(res = XvMCGetSurfaceStatus(disp, surf, &status));

            if (res != Success)
                VERBOSE(VB_PLAYBACK, LOC_ERR + "SyncSurface(): " +
                        QString("XvMCGetSurfaceStatus %1").arg(res));
            if (status & XVMC_RENDERING)
            {
                X11S(XvMCFlushSurface(disp, surf));
                while (IsRendering(frame))
                    usleep(50);
            }
        }
    }
#endif // USING_XVMC
}

void VideoOutputXv::FlushSurface(VideoFrame* frame)
{ 
    (void)frame;
#ifdef USING_XVMC
    xvmc_render_state_t *render = GetRender(frame);
    if (render)
    {
        Display *disp     = render->disp;
        XvMCSurface *surf = render->p_surface;
        if (disp && IsRendering(frame))
            X11S(XvMCFlushSurface(disp, surf));
    }
#endif // USING_XVMC
}

static void SetFromEnv(bool &useXvVLD, bool &useXvIDCT, bool &useXvMC,
                       bool &useXVideo, bool &useShm)
{
    // can be used to force non-Xv mode as well as non-Xv/non-Shm mode
    if (getenv("NO_XVMC_VLD"))
        useXvVLD = false;
    if (getenv("NO_XVMC_IDCT"))
        useXvIDCT = false;
    if (getenv("NO_XVMC"))
        useXvVLD = useXvIDCT = useXvMC = false;
    if (getenv("NO_XV"))
        useXvVLD = useXvIDCT = useXvMC = useXVideo = false;
    if (getenv("NO_SHM"))
        useXVideo = useShm = false;
}

static void SetFromHW(Display *d, bool &useXvMC, bool &useXVideo, bool &useShm)
{
    // find out about XvMC support
    if (useXvMC)
    {
#ifdef USING_XVMC
        int mc_event, mc_err, ret;
        X11S(ret = XvMCQueryExtension(d, &mc_event, &mc_err));
        if (True != ret)
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + "XvMC output requested, "
                    "but is not supported by display.");
            useXvMC = false;
        }

        int mc_ver, mc_rel;
        X11S(ret = XvMCQueryVersion(d, &mc_ver, &mc_rel));
        if (Success == ret)
            VERBOSE(VB_PLAYBACK, LOC + "XvMC version: "<<mc_ver<<"."<<mc_rel);
#else // !USING_XVMC
        VERBOSE(VB_IMPORTANT, LOC_ERR + "XvMC output requested, "
                "but is not compiled into MythTV.");
        useXvMC = false;
#endif // USING_XVMC
    }

    // find out about XVideo support
    if (useXVideo)
    {
        uint p_ver, p_rel, p_req, p_event, p_err, ret;
        X11S(ret = XvQueryExtension(d, &p_ver, &p_rel,
                                    &p_req, &p_event, &p_err));
        if (Success != ret)
        {
            VERBOSE(VB_IMPORTANT, LOC_ERR + "XVideo output requested, "
                    "but is not supported by display.");
            useXVideo = false;
            useXvMC = false;
        }
    }

    if (useShm)
    {
        const char *dispname = DisplayString(d);
        if ((dispname) && (*dispname == ':'))
            X11S(useShm = (bool) XShmQueryExtension(d));
    }
}

static QString xvflags2str(int flags)
{
    QString str("");
    if (XvInputMask == (flags & XvInputMask))
        str.append("XvInputMask ");
    if (XvOutputMask == (flags & XvOutputMask))
        str.append("XvOutputMask ");
    if (XvVideoMask == (flags & XvVideoMask))
        str.append("XvVideoMask ");
    if (XvStillMask == (flags & XvStillMask))
        str.append("XvStillMask ");
    if (XvImageMask == (flags & XvImageMask))
        str.append("XvImageMask ");
    return str;
}

CodecID myth2av_codecid(MythCodecID codec_id,
                        bool& vld, bool& idct, bool& mc)
{
    vld = idct = mc = false;
    CodecID ret = CODEC_ID_NONE;
    switch (codec_id)
    {
        case kCodec_NONE:
            ret = CODEC_ID_NONE;
            break;

        case kCodec_MPEG1:
            ret = CODEC_ID_MPEG1VIDEO;
            break;
        case kCodec_MPEG2:
            ret = CODEC_ID_MPEG2VIDEO;
            break;
        case kCodec_H263:
            ret = CODEC_ID_H263;
            break;
        case kCodec_MPEG4:
            ret = CODEC_ID_MPEG4;
            break;

        case kCodec_MPEG1_XVMC:
            mc = true;
            ret = CODEC_ID_MPEG2VIDEO_XVMC;
            break;
        case kCodec_MPEG2_XVMC:
            mc = true;
            ret = CODEC_ID_MPEG2VIDEO_XVMC;
            break;
        case kCodec_H263_XVMC:
            VERBOSE(VB_IMPORTANT, "Error: XvMC H263 not supported by ffmpeg");
            break;
        case kCodec_MPEG4_XVMC:
            VERBOSE(VB_IMPORTANT, "Error: XvMC MPEG4 not supported by ffmpeg");
            break;

        case kCodec_MPEG1_IDCT:
            idct = mc = true;
            ret = CODEC_ID_MPEG2VIDEO_XVMC;
            break;
        case kCodec_MPEG2_IDCT:
            idct = mc = true;
            ret = CODEC_ID_MPEG2VIDEO_XVMC;
            break;
        case kCodec_H263_IDCT:
            VERBOSE(VB_IMPORTANT, "Error: XvMC-IDCT H263 not supported by ffmpeg");
            break;
        case kCodec_MPEG4_IDCT:
            VERBOSE(VB_IMPORTANT, "Error: XvMC-IDCT MPEG4 not supported by ffmpeg");
            break;

        case kCodec_MPEG1_VLD:
            vld = true;
            ret = CODEC_ID_MPEG2VIDEO_XVMC_VLD;
            break;
        case kCodec_MPEG2_VLD:
            vld = true;
            ret = CODEC_ID_MPEG2VIDEO_XVMC_VLD;
            break;
        case kCodec_H263_VLD:
            VERBOSE(VB_IMPORTANT, "Error: XvMC-VLD H263 not supported by ffmpeg");
            break;
        case kCodec_MPEG4_VLD:
            VERBOSE(VB_IMPORTANT, "Error: XvMC-VLD MPEG4 not supported by ffmpeg");
            break;
        default:
            VERBOSE(VB_IMPORTANT, QString("Error: MythCodecID %1 has not been "
                                          "added to myth2av_codecid")
                    .arg(codec_id));
            break;
    } // switch(codec_id)
    return ret;
}

#ifdef USING_XVMC
static QString ErrorStringXvMC(int val)
{
    QString str = "unrecognized return value";
    switch (val)
    {
        case Success:   str = "Success"  ; break;
        case BadValue:  str = "BadValue" ; break;
        case BadMatch:  str = "BadMatch" ; break;
        case BadAlloc:  str = "BadAlloc" ; break;
    }
    return str;
}

static xvmc_render_state_t *GetRender(VideoFrame *frame)
{
    if (frame)
        return (xvmc_render_state_t*) frame->buf;
    return NULL;
}
#endif // USING_XVMC

static void clear_xv_buffers(VideoBuffers &vbuffers,
                             int width, int height,
                             int xv_chroma)
{
    if ((GUID_I420_PLANAR == xv_chroma) ||
        (GUID_YV12_PLANAR == xv_chroma))
    {
        for (uint i = 0; i < vbuffers.allocSize(); i++)
        {
            unsigned char *data = vbuffers.at(i)->buf;
            bzero(data, width * height);
            memset(data + width * height, 127,
                   width * height / 2);
        }
    }
}

void ChromaKeyOSD::AllocImage(int i)
{
    X11L;
    XImage *shm_img =
        XShmCreateImage(videoOutput->XJ_disp,
                        DefaultVisual(videoOutput->XJ_disp,
                                      videoOutput->XJ_screen_num),
                        videoOutput->XJ_depth, ZPixmap, 0,
                        &shm_infos[i],
                        videoOutput->dispw, videoOutput->disph);
    uint size = shm_img->bytes_per_line * (shm_img->height+1) + 128;
    X11U;

    if (shm_img)
    {
        shm_infos[i].shmid = shmget(IPC_PRIVATE, size, IPC_CREAT|0777);
        if (shm_infos[i].shmid >= 0)
        {
            shm_infos[i].shmaddr = (char*) shmat(shm_infos[i].shmid, 0, 0);

            shm_img->data = shm_infos[i].shmaddr;
            shm_infos[i].readOnly = False;

            X11L;
            XShmAttach(videoOutput->XJ_disp, &shm_infos[i]);
            XSync(videoOutput->XJ_disp, False); // needed for FreeBSD?
            X11U;

            // Mark for delete immediately.
            // It won't actually be removed until after we detach it.
            shmctl(shm_infos[i].shmid, IPC_RMID, 0);
        }
    }

    img[i] = shm_img;
    bzero((vf+i), sizeof(VideoFrame));
    vf[i].buf = (unsigned char*) shm_infos[i].shmaddr;
    vf[i].codec  = FMT_ARGB32;
    vf[i].height = videoOutput->disph;
    vf[i].width  = videoOutput->dispw;
    vf[i].bpp    = 32;
}

void ChromaKeyOSD::FreeImage(int i)
{
    if (!img[i])
        return;

    X11L;
    XShmDetach(videoOutput->XJ_disp, &(shm_infos[i]));
    XFree(img[i]);
    img[i] = NULL;
    X11U;

    if (shm_infos[i].shmaddr)
        shmdt(shm_infos[i].shmaddr);
    if (shm_infos[i].shmid > 0)
        shmctl(shm_infos[0].shmid, IPC_RMID, 0);

    bzero((shm_infos+i), sizeof(XShmSegmentInfo));
    bzero((vf+i),        sizeof(VideoFrame));
}

void ChromaKeyOSD::Reinit(int i)
{
    // Make sure the buffer is the right size...
    bool resolution_changed = ((vf[i].height != videoOutput->disph) ||
                               (vf[i].width  != videoOutput->dispw));
    if (resolution_changed)
    {
        FreeImage(i);
        AllocImage(i);
    }

    uint key = videoOutput->xv_colorkey;
    uint bpl = img[i]->bytes_per_line;

    // create chroma key line
    char *cln = (char*) memalign(128, bpl + 128);
    bzero(cln, bpl);
    int j  = max(videoOutput->dispxoff - videoOutput->dispx, 0);
    int ej = min(videoOutput->dispxoff + videoOutput->dispwoff, vf[i].width);
    for (; j < ej; ++j)
        ((uint*)cln)[j] = key;

    // boboff assumes the smallest interlaced resolution is 480 lines - 5%
    int boboff = (int)round(((double)videoOutput->disphoff) / 456 - 0.00001);
    boboff = (videoOutput->m_deinterlacing &&
              videoOutput->m_deintfiltername == "bobdeint") ? boboff : 0;

    // calculate beginning and end of chromakey
    int cstart = min(max(videoOutput->dispyoff + boboff, 0), vf[i].height - 1);
    int cend   = min(max(videoOutput->dispyoff + videoOutput->disphoff, 0),
                     vf[i].height);

    // Paint with borders and chromakey
    char *buf = shm_infos[i].shmaddr;
    int dispy = min(max(videoOutput->dispy, 0), vf[i].height - 1);

    VERBOSE(VB_PLAYBACK, LOC + "cstart: "<<cstart<<"  cend: "<<cend);
    VERBOSE(VB_PLAYBACK, LOC + " dispy: "<<dispy <<" disph: "<<vf[i].height);

    if (cstart > dispy)
        bzero(buf + (dispy * bpl), (cstart - dispy) * bpl);
    for (j = cstart; j < cend; ++j)
        memcpy(buf + (j*bpl), cln, bpl);
    if (cend < vf[i].height)
        bzero(buf + (cend * bpl), (vf[i].height - cend) * bpl);

    free(cln);
}

/** \fn ChromaKeyOSD::ProcessOSD(OSD*)
 * 
 *  \return true if we need a repaint, false otherwise
 */
bool ChromaKeyOSD::ProcessOSD(OSD *osd)
{
    OSDSurface *osdsurf = NULL;
    if (osd)
        osdsurf = osd->Display();

    int next = (current+1) & 0x1;
    if (!osdsurf && current >= 0)
    {
        Reset();
        return true;
    }
    else if (!osdsurf || (revision == osdsurf->GetRevision()))
        return false;

    // first create a blank frame with the chroma key
    Reinit(next);

    // then blend the OSD onto it
    unsigned char *buf = (unsigned char*) shm_infos[next].shmaddr;
    osdsurf->BlendToARGB(buf, img[next]->bytes_per_line, vf[next].height,
                         false/*blend_to_black*/, 16);

    // then set it as the current OSD image
    revision = osdsurf->GetRevision();
    current  = next;

    return true;
}
