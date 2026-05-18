/**
 ****************************************************************************************************
 * @file        photoviewer.h
 * @brief       Photo slideshow viewer for DNESP32S3
 *              - Reads JPEG images from SD card (/sdcard/PHOTOS/)
 *              - Auto-plays with configurable interval
 *              - Touch: tap=pause/resume, swipe left=prev, swipe right=next
 ****************************************************************************************************
 */

#ifndef __PHOTOVIEWER_H
#define __PHOTOVIEWER_H

/**
 * @brief  Start the photo viewer (call from menu or directly)
 *         Mounts SD card, scans for images, displays slideshow.
 */
void photoviewer_start(void);

/**
 * @brief  Called by menu_go_back() equivalent to return from photo viewer
 */
void photoviewer_stop(void);

#endif /* __PHOTOVIEWER_H */
