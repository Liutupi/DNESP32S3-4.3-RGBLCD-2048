#ifndef XIAOZHI_HEADLESS_H
#define XIAOZHI_HEADLESS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动小智AI无头模式
 * 关闭LCD屏幕，初始化音频，开始语音对话
 */
void xiaozhi_headless_start(void);

/**
 * 检查小智AI是否在无头模式运行
 * @return true 如果正在运行
 */
bool xiaozhi_headless_is_running(void);

/**
 * 停止小智AI无头模式
 * 停止音频，恢复LCD屏幕
 */
void xiaozhi_headless_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* XIAOZHI_HEADLESS_H */
