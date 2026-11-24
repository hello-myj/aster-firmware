/* SPDX-License-Identifier: MIT. Included by the native Launcher in AI builds. */
static bool ai_enabled, ai_loaded;
static atomic_bool ai_online = true;
static int ai_drawn = -1;
static int dialog_drawn = -1;
static lv_obj_t *ai_answer_label;
static unsigned ai_drawn_revision;
static uint64_t ai_text_time;
static struct dialog_view ai_view;
static const char ai_id[] = "aster.ai";
extern const uint8_t ai_whale_pixels[] asm("_binary_aster_whale_bin_start");
enum { AI_WHALE_WIDTH = 144, AI_WHALE_HEIGHT = 96 };
/* Scaling needs the full source image even when only one display strip is dirty. */
static const lv_image_dsc_t ai_whale = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8,
               .w = AI_WHALE_WIDTH, .h = AI_WHALE_HEIGHT, .stride = AI_WHALE_WIDTH * 2},
    .data_size = AI_WHALE_WIDTH * AI_WHALE_HEIGHT * 3, .data = ai_whale_pixels,
};

static void ai_float(void *image, int32_t offset)
{
    lv_obj_set_style_translate_y(image, offset, 0);
}

static const char *dialog_error(const struct dialog_view *v)
{
    switch (v->http) {
    case 401:
    case 403: return "服务拒绝了请求，请检查 API Key 和权限。";
    case 402: return "DeepSeek 账户余额不足。";
    case 429: return "服务请求过多，请稍后重试。";
    }
    switch (v->error) {
    case -ENODATA: return "没有听清，请靠近麦克风再说一次。";
    case -ENETDOWN: return "Wi-Fi 尚未连接。请在下拉设置中连接网络后重试。";
    case -ETIME: return "设备时钟还未同步。稍等片刻后再试语音识别。";
    case -EACCES: return "语音服务鉴权失败，请检查密钥和服务权限。";
    case -ENOENT: return "请先通过 USB 配置 API Key。";
    case -EBUSY: return "音频设备正被占用，请停止播放或录音后重试。";
    case -ENOMEM: return "当前可用内存不足。";
    }
    switch (v->failed_phase) {
    case DIALOG_ASR: return "未能完成语音识别，请再说一次。";
    case DIALOG_THINKING: return "模型暂时无法回答，请稍后再试。";
    case DIALOG_SYNTH:
    case DIALOG_SPEAKING: return "语音播报中断，请稍后再试。";
    default: return "语音对话启动失败，请重试。";
    }
}

static void build_dialog(enum dialog_phase phase)
{
    ai_drawn_revision = ai_dialog_revision();
    ai_dialog_snapshot(&ai_view);
    const struct dialog_view *v = &ai_view;
    const char *const titles[] = {"", "准备麦克风", "请讲，我在听", "可以接着说", "听到你了", "正在连接 Wi-Fi", "正在转写", "小鲸正在想",
                                 "正在生成语音", "小鲸正在说", "正在取消", "回答完毕", "对话未完成"};
    lv_obj_t *whale = lv_image_create(body);
    lv_image_set_src(whale, &ai_whale);
    lv_image_set_scale(whale, as(52) * 256 / AI_WHALE_WIDTH);
    lv_obj_set_pos(whale, ax(47) - AI_WHALE_WIDTH / 2, ay(82) - AI_WHALE_HEIGHT / 2);
    text(body, titles[phase], 82, 65, 218, 0x8ce0d3, 20, 258);
    lv_obj_t *card = box(body, 16, 115, 288, 237, 0x1a3346, 16);
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    const char *caption = v->answer[0] ? "小鲸" : v->question[0] ? "你说" : "小鲸助手";
    text(card, caption, 12, 10, 260, 0x96b4bc, 16, 205);
    const char *detail = v->answer[0] ? v->answer : v->question[0] ? v->question :
                         phase == DIALOG_NETWORK ? "已保留这句话，联网后会自动转写。" :
                         phase == DIALOG_ASR ? "正在将提问转为文字。" :
                         phase == DIALOG_CANCELLING ? "正在释放音频和网络连接。" :
                         "说完后停顿片刻，或点「说完了」。";
    bool recording = phase == DIALOG_RECORDING || phase == DIALOG_FOLLOWUP || phase == DIALOG_VOICED;
    if (recording) {
        lv_obj_t *indicator = box(card, 12, 50, 13, 13, 0x8ce0d3, 7);
        if (motion) {
            lv_anim_t pulse;
            lv_anim_init(&pulse);
            lv_anim_set_var(&pulse, indicator);
            lv_anim_set_exec_cb(&pulse, ai_float);
            lv_anim_set_values(&pulse, -as(2), as(2));
            lv_anim_set_duration(&pulse, 420);
            lv_anim_set_reverse_duration(&pulse, 420);
            lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&pulse);
        }
        text(card, phase == DIALOG_VOICED ? "已检测到人声" : "等待你开口", 32, 42, 220, 0x8ce0d3, 20, 256);
    }
    lv_obj_t *content = text(card, detail, 12, recording ? 98 : 48, 261,
                             0xe9f1ed, 16, 237);
    lv_label_set_long_mode(content, LV_LABEL_LONG_WRAP);
    ai_answer_label = content;
    if (phase == DIALOG_FAILED) {
        lv_obj_set_y(content, ay(48));
        lv_label_set_text(content, dialog_error(v));
        if (v->answer[0]) {
            lv_obj_update_layout(content);
            lv_obj_t *answer = text(card, v->answer, 12, 0, 261, 0xe9f1ed, 16, 237);
            lv_obj_set_y(answer, lv_obj_get_y(content) + lv_obj_get_height(content) + ay(10));
            lv_label_set_long_mode(answer, LV_LABEL_LONG_WRAP);
        }
    }
    bool terminal = phase == DIALOG_DONE || phase == DIALOG_FAILED;
    button(body, terminal ? "关闭 AI" : "结束对话", 16, 368, 132, 56, 0x32506b,
           terminal ? ACT_AI_TOGGLE : ACT_AI_END);
    const char *action = phase == DIALOG_VOICED ? "说完了" :
                         phase == DIALOG_DONE ? "新对话" : "再试一次";
    if (phase == DIALOG_VOICED || terminal)
        button(body, action, 158, 368, 146, 56, 0x326960, ACT_AI_REARM);
}

static void build_ai_companion(void)
{
    ai_answer_label = NULL;
    enum ai_voice_status state = ai_voice_status();
    ai_drawn = state;
    enum dialog_phase dialog = ai_dialog_phase();
    dialog_drawn = dialog;
    bool awake = state == AI_HEARD;
    lv_obj_set_style_bg_color(body, lv_color_hex(0x10202d), 0);
    text(body, "小鲸助手", 18, 9, 165, 0xe9f1ed, 20, 304);
    bool ready = ai_key_present();
    button(body, ready && ai_online ? "联网对话" : "离线唤醒", 191, 6, 113, 44, 0x32506b, ACT_AI_MODE);
    if (dialog != DIALOG_IDLE) {
        build_dialog(dialog);
        return;
    }
    text(body, "DEEPSEEK", 19, 48, 160, 0x96b4bc, 16, 160);
    lv_obj_t *sea = box(body, 16, 83, 288, 188, awake ? 0x244465 : 0x1a3346, 22);
    if (awake) {
        lv_obj_t *image = lv_image_create(sea);
        lv_image_set_src(image, &ai_whale);
        lv_image_set_scale(image, as(197) * 256 / AI_WHALE_WIDTH);
        lv_obj_center(image);
        if (motion) {
            lv_anim_t anim;
            lv_anim_init(&anim);
            lv_anim_set_var(&anim, image);
            lv_anim_set_exec_cb(&anim, ai_float);
            lv_anim_set_values(&anim, -as(3), as(3));
            lv_anim_set_duration(&anim, 1100);
            lv_anim_set_reverse_duration(&anim, 1100);
            lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
            lv_anim_start(&anim);
        }
    } else {
        const int bars[] = {8, 16, 27, 38, 22, 48, 31, 15, 9};
        for (unsigned i = 0; i < ARRAY_SIZE(bars); ++i)
            box(sea, 89 + 13*i, 95 - bars[i]/2, 7, bars[i],
                state == AI_LISTENING ? 0x8ce0d3 : 0x587586, 4);
    }
    const char *headline = "小鲸正在休息";
    const char *hint = "开启后，在桌面或此页说唤醒词。";
    if (awake) {
        headline = "我在！";
        hint = ready ? "点右上角「离线唤醒」切到联网对话。" : "通过 USB 配置 Key 后可联网对话。";
    } else if (ai_enabled) {
        switch (state) {
        case AI_LISTENING:
            headline = "请说「你好小智」";
            hint = ready && ai_online ? "唤醒后可向小鲸提问。" : "已开启离线聆听"; break;
        case AI_BUSY:
            headline = "麦克风正在使用"; hint = "停止播放或录音后，再试一次。"; break;
        case AI_FAILED:
            headline = "无法开始聆听"; hint = "请重试，详情可在系统日志查看。"; break;
        default:
            headline = "正在准备聆听"; hint = "正在载入本地唤醒模型。"; break;
        }
    }
    text(body, headline, 18, 284, 286, FG, 20, 284);
    text(body, hint, 18, 323, 283, 0xb2c8cf, 16, 212);
    bool talk = ai_enabled && ready && ai_online;
    bool retry = talk || awake || state == AI_BUSY || state == AI_FAILED;
    button(body, ai_enabled ? "关闭 AI" : "开启 AI", 16, 368, retry ? 132 : 288, 56,
           0x32506b, ACT_AI_TOGGLE);
    if (retry)
        button(body, talk ? "直接提问" : awake ? "再次聆听" : "重试", 158, 368, 146, 56,
               0x326960, ACT_AI_REARM);
}

static void ai_toggle(void)
{
    uint8_t value = !ai_enabled;
    if (aster_service_kv_set(ai_id, "enabled", &value, sizeof(value))) {
        notify("设置保存失败");
        return;
    }
    ai_enabled = value;
    ai_dialog_reset();
    ai_voice_stop();
    show_page(AI_COMPANION);
}

static void ai_action(void)
{
    enum dialog_phase dialog = ai_dialog_phase();
    if (dialog == DIALOG_VOICED) {
        ai_dialog_submit();
    } else if ((dialog == DIALOG_IDLE || dialog == DIALOG_DONE || dialog == DIALOG_FAILED) &&
               ai_enabled && ai_online && ai_key_present()) {
        ai_voice_stop();
        ai_dialog_start(AI_MICROPHONE);
    } else {
        ai_dialog_reset();
        ai_voice_stop();
    }
}

static bool ai_set_mode(bool next)
{
    if (next && !ai_key_present()) {
        notify("请先通过 USB 配置 API Key");
        return false;
    }
    uint8_t value = next;
    if (aster_service_kv_set(ai_id, "online", &value, 1)) {
        notify("设置保存失败");
        return false;
    }
    ai_online = next;
    ai_dialog_reset();
    ai_voice_stop();
    show_page(AI_COMPANION);
    return true;
}

static void ai_companion_tick(void)
{
    if (!ai_loaded) {
        uint8_t value = 0;
        size_t size = 0;
        ai_enabled = !aster_service_kv_get(ai_id, "enabled", &value, 1, &size) && size == 1 && value == 1;
        if (!aster_service_kv_get(ai_id, "online", &value, 1, &size) && size == 1)
            ai_online = value != 0;
        ai_loaded = true;
    }
    if (!ai_enabled)
        return;
    bool available = (page == HOME || page == AI_COMPANION) && !modal && !launching &&
                     !aster_wifi_settings_is_open() && !aster_bluetooth_settings_is_open();
    enum ai_voice_status state = ai_voice_status();
    if (!available) {
        ai_dialog_reset();
        if (state != AI_IDLE)
            ai_voice_stop();
        return;
    }
    enum dialog_phase dialog = ai_dialog_phase();
    bool wake_ready = dialog == DIALOG_IDLE || dialog == DIALOG_DONE || dialog == DIALOG_FAILED;
    if (state == AI_HEARD && wake_ready && (page == HOME || dialog != dialog_drawn ||
                                             ai_drawn != AI_HEARD)) {
        if (ai_online && ai_key_present()) {
            ai_voice_stop();
            ai_dialog_start(AI_MICROPHONE);
        }
        dialog = ai_dialog_phase();
    }
    if (dialog > DIALOG_IDLE && dialog < DIALOG_CANCELLING)
        power_idle.last_activity_ms = aster_os_uptime_ms();
    if (state == AI_IDLE && wake_ready && dialog != DIALOG_STARTING)
        ai_voice_start();
    if ((page == HOME && state == AI_HEARD) ||
        (page == AI_COMPANION && ((int)state != ai_drawn || (int)dialog != dialog_drawn)))
        show_page(AI_COMPANION);
    else if (page == AI_COMPANION && ai_answer_label &&
             (dialog == DIALOG_SYNTH || dialog == DIALOG_SPEAKING) &&
             ai_drawn_revision != ai_dialog_revision() && aster_os_uptime_ms() - ai_text_time >= 120) {
        ai_drawn_revision = ai_dialog_revision();
        ai_dialog_snapshot(&ai_view);
        set_text(ai_answer_label, ai_view.answer);
        ai_text_time = aster_os_uptime_ms();
    }
}
