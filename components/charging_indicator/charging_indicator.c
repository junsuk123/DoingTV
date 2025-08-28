// charging_indicator.c (옵션2 통합 버전, 최종)
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include "esp_log.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "st7789.h"

extern TFT_t *g_dev;

#define TAG             "CHARGE"
#define DEFAULT_VREF    1100
#define ADC1_CH         ADC1_CHANNEL_0
#define ADC_ATTEN       ADC_ATTEN_DB_11
/* ─ 분압 네트워크(회로 기준) ─ */
#define DIV_RTOP_K      200.0f   // R3 200k
#define DIV_RBOT_K      100.0f   // R7 100k
#define DIV_GAIN        ((DIV_RTOP_K + DIV_RBOT_K) / DIV_RBOT_K)   // 3.0f
#define ADC_FUDGE       0.985f   // 우선 0.985로 시작, 현장에서 ±0.5% 조정
/* 안전 클램프(노이즈 튀는 값 억제) */
#define VCLAMP_MIN      3.0f
#define VCLAMP_MAX      4.35f
#define SAMPLES         256      // 64 -> 256

typedef enum {
    CHG_STATE_UNKNOWN = 0,
    CHG_STATE_CHARGING,
    CHG_STATE_FULL,
    CHG_STATE_NOT_CHARGING
} chg_state_t;

typedef struct {
    float ema_alpha_v;
    float ema_alpha_dvdt;
    float v_full, v_full_hard;
    float dvdt_chg_on, dvdt_chg_off, dvdt_flat;
    float v_notchg;
    uint32_t hold_full_ms, hold_chg_ms, hold_notchg_ms;
    uint32_t relax_ms_after_detach;
} chg_cfg_t;

typedef struct {
    float v_raw, v_ema, dvdt_ema;
    float dvdt_inst;
    chg_state_t state, last_stable_state;
    uint32_t ms_in_state, ms_since_detach;
    float v_min, v_max;
} chg_metrics_t;

static const chg_cfg_t CDEF = {
    .ema_alpha_v    = 0.10f,
    .ema_alpha_dvdt = 0.08f,
    .v_full         = 4.18f,   // 실측 보정 후 기준
    .v_full_hard    = 4.22f,
    .dvdt_chg_on    = 1.0e-3f,
    .dvdt_chg_off   = 3.0e-4f,
    .dvdt_flat      = 1.0e-4f,
    .v_notchg       = 4.12f,
    .hold_full_ms   = 60000,
    .hold_chg_ms    = 15000,
    .hold_notchg_ms = 15000,
    .relax_ms_after_detach = 90000
};


static chg_cfg_t C;
static chg_metrics_t M;
static int inited_sm = 0;

static esp_adc_cal_characteristics_t adc_chars;

static void init_adc(void){
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CH, ADC_ATTEN);
    esp_adc_cal_value_t src = esp_adc_cal_characterize(
        ADC_UNIT_1, ADC_ATTEN, ADC_WIDTH_BIT_12, DEFAULT_VREF, &adc_chars);
    ESP_LOGI(TAG, "ADC cal src=%d vref=%" PRIu32 "mV", (int)src, adc_chars.vref);
}

static float read_batt_v(void){
    /* 고임피던스 분압 → 샘플/홀드 캡 프리차지용 더미 리드 */
    (void)adc1_get_raw(ADC1_CH);
    (void)adc1_get_raw(ADC1_CH);
    (void)adc1_get_raw(ADC1_CH);
    (void)adc1_get_raw(ADC1_CH);

    uint32_t raw = 0;
    for (int i = 0; i < SAMPLES; i++) raw += adc1_get_raw(ADC1_CH);
    raw /= SAMPLES;

    /* ADC 핀 전압(mV) → 배터리 전압(분압 역계수 × 보정) */
    uint32_t mv = esp_adc_cal_raw_to_voltage(raw, &adc_chars);  // ADC핀 기준 mV
    float vbatt = (mv / 1000.0f) * DIV_GAIN * ADC_FUDGE;

    /* 물리 범위로 클램프 */
    if (vbatt < VCLAMP_MIN) vbatt = VCLAMP_MIN;
    if (vbatt > VCLAMP_MAX) vbatt = VCLAMP_MAX;
    return vbatt;
}


static inline const char* chg_state_str(chg_state_t s){
    switch (s) {
        case CHG_STATE_CHARGING:     return "CHARGING";
        case CHG_STATE_FULL:         return "FULL";
        case CHG_STATE_NOT_CHARGING: return "NOT_CHARGING";
        default: return "UNKNOWN";
    }
}

static chg_state_t decide_target(void){
    if (M.v_ema >= C.v_full_hard && fabsf(M.dvdt_ema) < C.dvdt_flat*2) return CHG_STATE_FULL;
    if (M.v_ema >= C.v_full && fabsf(M.dvdt_ema) < C.dvdt_flat)        return CHG_STATE_FULL;
    if (M.dvdt_ema >= C.dvdt_chg_on)                                   return CHG_STATE_CHARGING;
    if (M.dvdt_ema <= 0.0f || M.v_ema <= C.v_notchg)                   return CHG_STATE_NOT_CHARGING;
    return (M.last_stable_state == CHG_STATE_UNKNOWN) ? CHG_STATE_NOT_CHARGING
                                                      : M.last_stable_state;
}

static void sm_init(void){
    C = CDEF;
    M = (chg_metrics_t){0};
    M.v_min =  99.f;
    M.v_max = -99.f;
    M.state = CHG_STATE_UNKNOWN;
    M.last_stable_state = CHG_STATE_UNKNOWN;
    inited_sm = 1;
}

static void sm_update(float v, uint32_t dt_ms){
    if (!inited_sm) sm_init();

    if (M.v_ema == 0.f) M.v_ema = v;
    M.v_raw = v;
    M.v_ema = C.ema_alpha_v * v + (1.f - C.ema_alpha_v) * M.v_ema;

    static float prev_v = 0.f; static int have_prev = 0;
    if (!have_prev) { prev_v = v; have_prev = 1; }
    float dv = v - prev_v; prev_v = v;

    float dt_s = (dt_ms > 0 ? dt_ms : 1) / 1000.0f;
    float dvdt_instant = dv / dt_s;
    M.dvdt_ema = C.ema_alpha_dvdt * dvdt_instant + (1.f - C.ema_alpha_dvdt) * M.dvdt_ema;
    M.dvdt_inst = dvdt_instant; 

    if (M.v_ema < M.v_min) M.v_min = M.v_ema;
    if (M.v_ema > M.v_max) M.v_max = M.v_ema;

    chg_state_t target = decide_target();

    bool promote = false;
    switch (target) {
        case CHG_STATE_FULL:
            promote = (M.ms_in_state >= C.hold_full_ms) || (M.v_ema >= C.v_full_hard);
            break;
        case CHG_STATE_CHARGING:
            if (M.dvdt_ema >= C.dvdt_chg_off) promote = (M.ms_in_state >= C.hold_chg_ms);
            break;
        case CHG_STATE_NOT_CHARGING:
            promote = (M.ms_in_state >= C.hold_notchg_ms) &&
                      (M.ms_since_detach >= C.relax_ms_after_detach ||
                       M.last_stable_state != CHG_STATE_CHARGING);
            break;
        default:
            promote = true; break;
    }

    static chg_state_t prev_target = CHG_STATE_UNKNOWN;
    if (target != prev_target) {
        M.ms_in_state = 0;
        prev_target = target;
    } else {
        M.ms_in_state += dt_ms;
    }

    if (promote && target != M.last_stable_state) {
        if (M.last_stable_state == CHG_STATE_CHARGING && target != CHG_STATE_CHARGING) {
            M.ms_since_detach = 0;
        }
        M.last_stable_state = target;
    } else {
        if (M.last_stable_state != CHG_STATE_CHARGING) {
            if (M.ms_since_detach < 0xFFFFFFF0) M.ms_since_detach += dt_ms;
        }
    }

    M.state = (M.last_stable_state == CHG_STATE_UNKNOWN) ? target : M.last_stable_state;
}

// 공개 API
void charging_indicator_init(void){
    init_adc();
    sm_init();
}

void charging_indicator_update(uint32_t dt_ms){
    float v = read_batt_v();
    sm_update(v, dt_ms);

    ESP_LOGI(TAG, "V=%.3f V  Vraw=%.3f  Vema=%.3f  dV/dt=%.5f (inst=%.5f) state=%s hold=%" PRIu32 " ms",
         v, M.v_raw, M.v_ema, M.dvdt_ema, M.dvdt_inst, chg_state_str(M.state), (uint32_t)M.ms_in_state);


    if (!g_dev) return;

    switch (M.state) {
        case CHG_STATE_CHARGING:
            lcdBacklightOff(g_dev);
            spi_master_write_command(g_dev, 0x28);
            break;
        case CHG_STATE_FULL:
            lcdBacklightOn(g_dev);
            spi_master_write_command(g_dev, 0x29);
            break;
        case CHG_STATE_NOT_CHARGING:
        default:
            lcdBacklightOn(g_dev);
            spi_master_write_command(g_dev, 0x29);
            break;
    }
}
