/**
 * PS Move API - An interface for the PS Move Motion Controller
 * Copyright (c) 2012 Thomas Perl <m@thp.io>
 * Copyright (c) 2012 Benjamin Venditti <benjamin.venditti@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 **/

#include "psmove_config.h"
#include "camera_control.h"
#include "psmove_tracker.h"

#include "../psmove_private.h"

#include <stdio.h>
#include <stdint.h>

#if defined(__linux)
#include <sys/stat.h>
#include <glob.h>
#endif

#include "camera_control_private.h"

bool
camera_control_fallback_frame_layout(CameraControl *cc, int width, int height, struct CameraControlFrameLayout *layout)
{
    if (width == -1) {
        width = 640;
    }

    if (height == -1) {
        height = 480;
    }

    layout->capture_width = width;
    layout->capture_height = height;
    layout->crop_x = 0;
    layout->crop_x = 0;
    layout->crop_width = width;
    layout->crop_height = height;

    return true;
}

CameraControl *
camera_control_new_with_settings(int cameraID, int width, int height, int framerate)
{
	CameraControl* cc = (CameraControl*) calloc(1, sizeof(CameraControl));
	cc->cameraID = cameraID;

    if (width == -1) {
        width = psmove_util_get_env_int(PSMOVE_TRACKER_WIDTH_ENV);
    }

    if (height == -1) {
        height = psmove_util_get_env_int(PSMOVE_TRACKER_HEIGHT_ENV);
    }

    if (framerate == -1) {
        framerate = 60;
    }

#if defined(CAMERA_CONTROL_USE_PS3EYE_DRIVER)
    ps3eye_init();
    int cams = ps3eye_count_connected();

    if (cams <= cameraID) {
        free(cc);
        return NULL;
    }

    if (!camera_control_get_frame_layout(cc, width, height, &(cc->layout))) {
        free(cc);
        return NULL;
    }

    cc->eye = ps3eye_open(cameraID, cc->layout.capture_width, cc->layout.capture_height, framerate, PS3EYE_FORMAT_BGR);

    if (cc->eye == NULL) {
        free(cc);
        return NULL;
    }

    cc->framebgr = cvCreateImage(cvSize(cc->layout.capture_width, cc->layout.capture_height), IPL_DEPTH_8U, 3);
#else
    char *video = psmove_util_get_env_string(PSMOVE_TRACKER_FILENAME_ENV);

    if (video) {
        PSMOVE_INFO("Using '%s' as video input.", video);
        cc->capture = new cv::VideoCapture(video);
        psmove_free_mem(video);
    } else {
#if defined(__linux)
        // Remap camera ID based on available /dev/video* device nodes. This fixes
        // an issue when disconnecting a PS Eye camera during video capture, which
        // - when reconnected - might have an non-zero ID.
        glob_t g;
        if (glob("/dev/video*", 0, NULL, &g) == 0) {
            if (g.gl_pathc > (size_t)cc->cameraID) {
                if (sscanf(g.gl_pathv[cc->cameraID] + strlen("/dev/video"), "%d", &cc->cameraID) != 1) {
                    PSMOVE_WARNING("Could not determine camera ID from path '%s'", g.gl_pathv[cc->cameraID]);
                }
            }
            globfree(&g);
        }
#endif

        cc->capture = new cv::VideoCapture(cc->cameraID);

        if (cc->capture == NULL) {
            free(cc);
            return NULL;
        }

        if (!camera_control_get_frame_layout(cc, width, height, &(cc->layout))) {
            free(cc);
            return NULL;
        }

        cc->capture->set(cv::CAP_PROP_FRAME_WIDTH, cc->layout.capture_width);
        cc->capture->set(cv::CAP_PROP_FRAME_HEIGHT, cc->layout.capture_height);
    }
#endif
    cc->deinterlace = PSMove_False;

	return cc;
}

int
camera_control_count_connected()
{
#if defined(CAMERA_CONTROL_USE_PS3EYE_DRIVER)
    ps3eye_init();
    return ps3eye_count_connected();
#elif defined(__linux)
    int i = 0;
    glob_t g;
    if (glob("/dev/video*", 0, NULL, &g) == 0) {
        i = g.gl_pathc;
        globfree(&g);
    }
    return i;
#else
    PSMOVE_WARNING("Getting number of connected cameras not implemented");
    return -1;
#endif
}

void
camera_control_set_deinterlace(CameraControl *cc,
        enum PSMove_Bool enabled)
{
    psmove_return_if_fail(cc != NULL);

    cc->deinterlace = enabled;
}

void
camera_control_read_calibration(CameraControl* cc,
        char* intrinsicsFile, char* distortionFile)
{
#if CV_VERSION_MAJOR <= 3
    CvMat *intrinsic = (CvMat*) cvLoad(intrinsicsFile, 0, 0, 0);
    CvMat *distortion = (CvMat*) cvLoad(distortionFile, 0, 0, 0);

    if (cc->mapx) {
        cvReleaseImage(&cc->mapx);
    }
    if (cc->mapy) {
        cvReleaseImage(&cc->mapy);
    }

    if (intrinsic && distortion) {
        if (!cc->frame3chUndistort) {
            cc->frame3chUndistort = cvCloneImage(
                    camera_control_query_frame(cc));
        }

		cc->mapx = cvCreateImage(cvSize(cc->width, cc->height), IPL_DEPTH_32F, 1);
		cc->mapy = cvCreateImage(cvSize(cc->width, cc->height), IPL_DEPTH_32F, 1);

        cvInitUndistortMap(intrinsic, distortion, cc->mapx, cc->mapy);

        // TODO: Shouldn't we free intrinsic and distortion here?
    } else {
        PSMOVE_WARNING("No lens calibration files found.");
    }
#else
    PSMOVE_WARNING("Camera calibration not yet supported in OpenCV 4");
#endif
}

IplImage *
camera_control_query_frame( CameraControl* cc)
{
    IplImage *result = nullptr;

#if defined(CAMERA_CONTROL_USE_PS3EYE_DRIVER)
    // Get raw data pointer
    unsigned char *cvpixels;
    cvGetRawData(cc->framebgr, &cvpixels, 0, 0);

	// Grab frame to buffer
	ps3eye_grab_frame(cc->eye, cvpixels);

    result = cc->framebgr;
#else
    cv::Mat frame;
    if (cc->capture->read(frame)) {
        if (cc->frame != nullptr) {
            cvReleaseImage(&cc->frame);
        }

        IplImage tmp = cvIplImage(frame);

        cvSetImageROI(&tmp, cvRect(cc->layout.crop_x, cc->layout.crop_y, cc->layout.crop_width, cc->layout.crop_height));
        result = cvCreateImage(cvSize(cc->layout.crop_width, cc->layout.crop_height), IPL_DEPTH_8U, 3);

        cvCopy(&tmp, result);
        cc->frame = result;
    }
#endif

    if (cc->deinterlace == PSMove_True) {
        /**
         * Dirty hack follows:
         *  - Clone image
         *  - Hack internal variables to make an image of all odd lines
         **/
        IplImage *tmp = cvCloneImage(result);
        tmp->imageData += tmp->widthStep; // odd lines
        tmp->widthStep *= 2;
        tmp->height /= 2;

        /**
         * Use nearest-neighbor to be faster. In my tests, this does not
         * cause a speed disadvantage, and tracking quality is still good.
         *
         * This will scale the half-height image "tmp" to the original frame
         * size by doubling lines (so we can still do normal circle tracking).
         **/
        cvResize(tmp, result, CV_INTER_NN);

        /**
         * Need to revert changes in tmp from above, otherwise the call
         * to cvReleaseImage would cause a crash.
         **/
        tmp->height = result->height;
        tmp->widthStep = result->widthStep;
        tmp->imageData -= tmp->widthStep; // odd lines
        cvReleaseImage(&tmp);
    }

    // undistort image
    if (cc->mapx && cc->mapy) {
        cvRemap(result, cc->frame3chUndistort,
                cc->mapx, cc->mapy,
                CV_INTER_LINEAR | CV_WARP_FILL_OUTLIERS,
                cvScalarAll(0));
        result = cc->frame3chUndistort;
    }


#if defined(CAMERA_CONTROL_DEBUG_CAPTURED_IMAGE)
    cvShowImage("camera input", result);
    cvWaitKey(1);
#endif

    return result;
}

void
camera_control_delete(CameraControl* cc)
{
#if defined(CAMERA_CONTROL_USE_PS3EYE_DRIVER)
    cvReleaseImage(&cc->framebgr);

    ps3eye_close(cc->eye);
    ps3eye_uninit();
#else
    if (cc->frame != nullptr) {
        cvReleaseImage(&cc->frame);
    }

    // linux, others and windows opencv only
    delete cc->capture;
    cc->capture = nullptr;
#endif

    if (cc->frame3chUndistort) {
        cvReleaseImage(&cc->frame3chUndistort);
    }

    if (cc->mapx) {
        cvReleaseImage(&cc->mapx);
    }

    if (cc->mapy) {
        cvReleaseImage(&cc->mapy);
    }

    free(cc);
}

void
camera_control_ps3eyedriver_set_parameters(CameraControl* cc, float exposure, bool mirror)
{
#if defined(CAMERA_CONTROL_USE_PS3EYE_DRIVER)
    ps3eye_set_parameter(cc->eye, PS3EYE_AUTO_GAIN, 0);
    ps3eye_set_parameter(cc->eye, PS3EYE_AUTO_WHITEBALANCE, 0);
    ps3eye_set_parameter(cc->eye, PS3EYE_EXPOSURE, int(0x1FF * std::min(1.f, std::max(0.f, exposure))));
    ps3eye_set_parameter(cc->eye, PS3EYE_GAIN, 0);
    ps3eye_set_parameter(cc->eye, PS3EYE_HFLIP, mirror);
#endif
}

struct PSMoveCameraInfo
camera_control_fallback_get_camera_info(CameraControl *cc)
{
#if defined(CAMERA_CONTROL_USE_PS3EYE_DRIVER)
    return PSMoveCameraInfo {
        "PS3 Eye",
        "PS3EYEDriver",
        cc->layout.crop_width,
        cc->layout.crop_height,
    };
#else
    return PSMoveCameraInfo {
        "Unknown camera",
        "Unknown API",
        cc->layout.crop_width,
        cc->layout.crop_height,
    };
#endif
}
