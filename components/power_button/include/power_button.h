#ifndef POWER_BUTTON_H
#define POWER_BUTTON_H

#include <stdbool.h>

// "롱프레스(3초)"가 감지되었을 때 호출할 콜백 타입
typedef void (*power_button_cb_t)(void);

/**
 * @brief 전원 버튼 컴포넌트 초기화
 * @param on_press_cb  : 버튼 롱프레스(3초) 시 호출될 콜백 함수 포인터. 
 *                       NULL 입력 시 콜백 없이 기본 OFF 동작만 수행.
 * @note  초기화 직후 SYS_EN을 HIGH로 세팅하여 전원을 유지(lock)합니다.
 *        이후 사용자가 PWR 버튼을 3초간 누르면 콜백을 호출하고, 
 *        SYS_EN을 LOW로 내려서 전원을 OFF 합니다.
 */
void power_button_init(power_button_cb_t on_press_cb);

#endif // POWER_BUTTON_H
