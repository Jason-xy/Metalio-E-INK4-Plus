#include "inkmeter_screen.h"
#include "display_orient.h"
#include "epd_refresh.h"
#include "fontpack_lvgl.h"
#include "inkmeter/inkmeter.h"
#include "lv_adapter_display.h"
#include "power_policy.h"
#include "screen_common.h"
#include "ui_qrcode.h"
#include "vk_key_handler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <optional>

LV_FONT_DECLARE(inkmeter_digits_font);
LV_FONT_DECLARE(inkmeter_labels_font);
LV_FONT_DECLARE(inkmeter_tokens_font);

namespace {
using namespace inkmeter;
lv_obj_t *screen = nullptr, *body = nullptr, *status_label = nullptr,
         *footer = nullptr, *dialog = nullptr;
lv_timer_t *timer = nullptr;
std::optional<PowerNeedHold> stay_awake;
bool loaded = false, sleeping = false, scrolling = false, repaint_pending = false;
int page = 0, metric = 0;
bool sort_cost = false;
uint64_t seen_revision = 0;
int seen_config_minute = -1;
std::string seen_pair_state;
std::vector<lv_point_precise_t> spark_points;
lv_obj_t *model_list = nullptr;
lv_obj_t *quota_list = nullptr;
int model_scroll = 0, quota_scroll = 0, quota_group = 0;
ScreenPaintCoalesce paint;

const lv_font_t *Small() {
    const auto *font = fontpack_lv_font_get(25, 2);
    return font ? font : &inkmeter_labels_font;
}
const lv_font_t *Normal() {
    const auto *font = fontpack_lv_font_ui();
    return font ? font : Small();
}
const lv_font_t *Latin() { return Small(); }
const lv_font_t *NumberFont() { return &lv_font_montserrat_42; }
const lv_font_t *TokenFont() { return &inkmeter_tokens_font; }
std::string UserName(const View &v) { return UserDisplayName(nullptr, v.user_name, v.user_id); }
std::string Integer(uint64_t value) {
    auto text = std::to_string(value);
    for (size_t pos = text.size(); pos > 3; pos -= 3)
        text.insert(pos - 3, ",");
    return text;
}
lv_color_t Gray(int value) {
    return lv_color_hex((value << 16) | (value << 8) | value);
}
lv_obj_t *Box(lv_obj_t *parent, int x, int y, int w, int h, bool border = false) {
    auto *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    if (border) {
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, Gray(185), 0);
        lv_obj_set_style_radius(o, 10, 0);
    }
    return o;
}
lv_obj_t *Label(lv_obj_t *parent, const std::string &text, int x, int y, int w,
                const lv_font_t *font = nullptr) {
    if(text=="—")font=Small();
    if (font == Latin() && std::any_of(text.begin(), text.end(), [](unsigned char c) { return c >= 128; }))
        font = Small();
    auto *o = lv_label_create(parent);
    lv_label_set_text(o, text.c_str());
    lv_obj_set_pos(o, x, y);
    if (w > 0) {
        lv_obj_set_width(o, w);
        lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
    }
    lv_obj_set_style_text_font(o, font ? font : Small(), 0);
    lv_obj_set_style_text_color(o, lv_color_black(), 0);
    return o;
}
void Rule(lv_obj_t *parent, int x, int y, int w, int = 185) {
    auto *o = Box(parent, x, y, w, 1);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}
void Divider(lv_obj_t *parent, int x, int y, int h) {
    auto *o = Box(parent, x, y, 1, h);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}
void ScheduleRender() {
    repaint_pending = true;
    ScreenPaintCoalesceRequest(&paint);
}
void CaretDraw(lv_event_t *e) {
    lv_area_t area; lv_obj_get_coords(static_cast<lv_obj_t *>(lv_event_get_target(e)), &area);
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d); d.bg_color = lv_color_black(); d.bg_opa = LV_OPA_COVER;
    for (int y = 0; y < 7; ++y) {
        lv_area_t row = {area.x1 + y, area.y1 + y, area.x2 - y, area.y1 + y};
        lv_draw_rect(lv_event_get_layer(e), &d, &row);
    }
}
lv_obj_t *Button(lv_obj_t *parent, const std::string &text, int x, int y, int w, int h,
                 lv_event_cb_t callback, intptr_t arg = 0, bool filled = false, bool quiet = false) {
    auto *o = lv_button_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    if (filled || !quiet) {
        const bool navigation = parent == footer;
        const int ix = navigation ? std::min(52, w / 5) : 2;
        const int iy = navigation ? 14 : h >= 70 ? 10 : 4;
        auto *background = Box(o, ix, iy, w - 2 * ix, h - 2 * iy);
        lv_obj_set_style_radius(background, 8, 0);
        lv_obj_set_style_bg_color(background, filled ? lv_color_black() : Gray(238), 0);
        lv_obj_set_style_bg_opa(background, LV_OPA_COVER, 0);
    }
    const bool caret = text.size() >= 4 && text.compare(text.size() - 4, 4, " ▾") == 0;
    auto *l = Label(o, caret ? text.substr(0, text.size() - 4) : text, 0, 0, w - (caret ? 45 : 20));
    lv_obj_set_height(l, std::min(h - 12, int(Small()->line_height)));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, filled ? lv_color_white() : lv_color_black(), 0);
    lv_obj_center(l);
    if (caret) {
        lv_obj_set_x(l, -7);
        lv_point_t caption_size;
        const auto caption = text.substr(0, text.size() - 4);
        lv_text_get_size(&caption_size, caption.c_str(), Small(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        auto *arrow = Box(o, std::min(w - 20, (w + int(caption_size.x)) / 2 + 1), (h - 7) / 2, 14, 7);
        lv_obj_add_event_cb(arrow, CaretDraw, LV_EVENT_DRAW_MAIN, nullptr);
    }
    if (callback)
        lv_obj_add_event_cb(o, callback, LV_EVENT_CLICKED, reinterpret_cast<void *>(arg));
    return o;
}
void HideDialog() {
    if (dialog) {
        lv_obj_delete(dialog);
        dialog = nullptr;
    }
}
void SelectPage(int next) {
    next = std::max(0, std::min(3, next));
    if (page == next)
        return;
    if (model_list)
        model_scroll = lv_obj_get_scroll_y(model_list);
    if (quota_list)
        quota_scroll = lv_obj_get_scroll_y(quota_list);
    page = next;
    HideDialog();
    if (page == 2)
        ReadQuotaSnapshots();
    EpdRequestScreenChange();
    ScheduleRender();
}
void Page(lv_event_t *e) {
    SelectPage(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}
void Refresh(lv_event_t *) {
    if (page == 2)
        ReadQuotaSnapshots();
    else
        RefreshUsage();
}
void ChangeRange(lv_event_t *e) {
    const int value = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    model_scroll = 0;
    if (model_list)
        lv_obj_scroll_to_y(model_list, 0, LV_ANIM_OFF);
    SelectRange(static_cast<Range>(value));
    ScheduleRender();
}
void CycleRange(lv_event_t *) {
    auto v = GetView();
    SelectRange(static_cast<Range>((int(v.usage.range) + 1) % 3));
    ScheduleRender();
}
void Metric(lv_event_t *e) {
    metric = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    ScheduleRender();
}
void Sort(lv_event_t *) {
    sort_cost = !sort_cost;
    model_scroll = 0;
    if (model_list)
        lv_obj_scroll_to_y(model_list, 0, LV_ANIM_OFF);
    ScheduleRender();
}
void PickUser(lv_event_t *e) {
    const auto id = *static_cast<int64_t *>(lv_event_get_user_data(e));
    HideDialog();
    SelectUser(id);
    ScheduleRender();
}
lv_obj_t *IdButton(lv_obj_t *parent, const std::string &text, int x, int y, int w, int h,
                   lv_event_cb_t callback, int64_t id, bool filled = false) {
    auto *button = Button(parent, text, x, y, w, h, nullptr, 0, filled);
    auto *value = new (std::nothrow) int64_t(id);
    if (!value) {
        lv_obj_add_state(button, LV_STATE_DISABLED);
        return button;
    }
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, value);
    lv_obj_add_event_cb(
        button, [](lv_event_t *e) { delete static_cast<int64_t *>(lv_event_get_user_data(e)); },
        LV_EVENT_DELETE, value);
    return button;
}
void UsersPage(lv_event_t *e) {
    const int p = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    BrowseUsers(p);
}
void Scroll(lv_event_t *e) {
    const auto code = lv_event_get_code(e);
    scrolling=code==LV_EVENT_SCROLL_BEGIN;
    EpdSetUiScrolling(scrolling);
    if (code == LV_EVENT_SCROLL_END){
        EpdRequestUiScrollSettle();
        if(repaint_pending || Revision()!=seen_revision)ScheduleRender();
    }
}
void MakeScrollable(lv_obj_t *o) {
    lv_obj_add_flag(o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(o, Scroll, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_add_event_cb(o, Scroll, LV_EVENT_SCROLL_END, nullptr);
    lv_obj_set_style_pad_bottom(o, 12, 0);
}
void Gesture(lv_event_t *e) {
    if (dialog || sleeping || scrolling)
        return;
    auto *input = lv_event_get_indev(e);
    if (!input)
        return;
    const auto dir = lv_indev_get_gesture_dir(input);
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT)
        SelectPage(page + (dir == LV_DIR_LEFT ? 1 : -1));
}
void ShowUsers(lv_event_t *event) {
    HideDialog();
    dialog = Box(screen, 0, 0, DisplayUiLogW(), DisplayUiLogH());
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
    lv_obj_add_flag(dialog, LV_OBJ_FLAG_CLICKABLE);
    const int x = std::max(24, (DisplayUiLogW() - 852) / 2), width = DisplayUiLogW() - 2 * x;
    Label(dialog, "选择监控用户", x, 43, width - 120, Normal());
    Button(dialog, "关闭", x + width - 104, 20, 104, 84, [](lv_event_t *) { HideDialog(); ScheduleRender(); });
    Rule(dialog, x, 116, width);
    auto *list = Box(dialog, x, 132, width, DisplayUiLogH() - 244);
    MakeScrollable(list);
    const auto v = GetView();
    int y = 0;
    for (const auto &u : v.users) {
        auto *button = IdButton(list, UserDisplayName(nullptr, u.second, u.first), 0, y, width - 12, 88, PickUser, u.first, u.first == v.user_id);
        if (v.busy) lv_obj_add_state(button, LV_STATE_DISABLED);
        y += 100;
    }
    if (v.users.empty()) Label(list, v.busy ? "正在读取用户列表…" : "暂无用户，请先配置站点", 12, 20, width - 24);
    if (v.user_page > 1) Button(dialog, "上一页", x, DisplayUiLogH() - 100, 160, 84, UsersPage, v.user_page - 1);
    if (v.more_users) Button(dialog, "下一页", x + width - 160, DisplayUiLogH() - 100, 160, 84, UsersPage, v.user_page + 1);
    if (event) BrowseUsers(v.user_page);
}
std::string RangeTitle(Range r) {
    return r == Range::Today ? "今日" : r == Range::Week ? "近7天" : "近30天";
}
void Title(const char *title, const char *) { Label(body, title, 0, 24, 350, Normal()); }
void Bar(lv_obj_t *parent, int x, int y, int w, int h, double percent, int = 0) {
    auto *track = Box(parent, x, y, w, h);
    lv_obj_set_style_border_width(track, 1, 0);
    lv_obj_set_style_border_color(track, lv_color_black(), 0);
    const int width = std::max(0, int((w - 2) * std::min(100.0, std::max(0.0, percent)) / 100));
    if (width) {
        auto *fill = Box(track, 1, 1, width, std::max(1, h - 2));
        lv_obj_set_style_bg_color(fill, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    }
}
void PatternDraw(lv_event_t *e) {
    auto *layer = lv_event_get_layer(e);
    lv_area_t area;
    lv_obj_get_coords(static_cast<lv_obj_t *>(lv_event_get_target(e)), &area);
    const int kind = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_black(); d.bg_opa = LV_OPA_COVER;
    const int width = lv_area_get_width(&area), height = lv_area_get_height(&area);
    if (kind == 1) {
        for (int y = area.y1; y <= area.y2; y += 4) {
            lv_area_t a = {area.x1, y, area.x2, std::min<int32_t>(area.y2, y + 1)};
            lv_draw_rect(layer, &d, &a);
        }
    } else if (kind == 2) {
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line); line.color = lv_color_black(); line.width = 2;
        for (int offset = -height; offset < width; offset += 8) {
            const int start = std::max(0, -offset), end = std::min(height - 1, width - 1 - offset);
            if (start > end) continue;
            line.p1 = {lv_value_precise_t(area.x1 + offset + start), lv_value_precise_t(area.y2 - start)};
            line.p2 = {lv_value_precise_t(area.x1 + offset + end), lv_value_precise_t(area.y2 - end)};
            lv_draw_line(layer, &line);
        }
    } else if (kind == 3) {
        for (int y = area.y1 + 2; y <= area.y2; y += 7)
            for (int x = area.x1 + 2; x <= area.x2; x += 7) {
                lv_area_t a = {x, y, std::min<int32_t>(area.x2, x + 2), std::min<int32_t>(area.y2, y + 2)};
                lv_draw_rect(layer, &d, &a);
            }
    }
}
lv_obj_t *Pattern(lv_obj_t *parent, int x, int y, int w, int h, int kind) {
    auto *o = Box(parent, x, y, w, h);
    lv_obj_set_style_bg_color(o, kind ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    if (kind) lv_obj_add_event_cb(o, PatternDraw, LV_EVENT_DRAW_MAIN, reinterpret_cast<void *>(intptr_t(kind)));
    return o;
}
int TextHeight(const std::string &text, int width, const lv_font_t *font, int line_space = 0) {
    if (font == Latin() && std::any_of(text.begin(), text.end(), [](unsigned char c) { return c >= 128; }))
        font = Small();
    lv_point_t size{};
    lv_text_get_size(&size, text.c_str(), font, 0, line_space, width, LV_TEXT_FLAG_NONE);
    return size.y;
}
bool MetricKnown(const Counts &c, int selected) {
    return selected == 1 ? c.cost_known : selected == 2 ? c.requests_known : c.tokens_known;
}
std::string MetricTotal(const Usage &u) {
    if (!u.valid || !MetricKnown(u.totals, metric))
        return "—";
    return metric == 1 ? "$" + Money(u.totals.cost)
           : metric == 2 ? Integer(u.totals.requests) : Compact(u.totals.Total());
}
void Plot(lv_obj_t *parent, const Usage &u, int x, int y, int width, int height, bool cumulative) {
    auto *plot = Box(parent, x, y, width, height);
    if (!cumulative) Rule(plot, 0, height - 40, width);
    if (!u.valid || u.buckets.empty()) {
        Label(plot, u.valid ? "此范围暂无用量" : cumulative ? "请先完成配置" : "尚无用量数据", 20, 35, width - 40);
        return;
    }
    const size_t n = u.buckets.size();
    const int graph_h = height - 40;
    if (cumulative) {
        if (!u.totals.tokens_known ||
            !std::all_of(u.buckets.begin(), u.buckets.end(), [](const Bucket &b) { return b.counts.tokens_known; })) {
            Label(plot, "分桶数据不完整，无法计算累计趋势", 20, 35, width - 40);
            return;
        }
        spark_points.clear();
        spark_points.push_back({0, lv_value_precise_t(graph_h)});
        double running = 0;
        for (size_t i = 0; i < n; i++) {
            running += u.buckets[i].counts.Total();
            spark_points.push_back(
                {lv_value_precise_t((i + 1) * (width - 5) / n),
                 lv_value_precise_t(graph_h -
                                    (u.totals.Total() ? running / u.totals.Total() : 0) * (graph_h - 8))});
        }
        auto *line = lv_line_create(plot);
        lv_obj_set_pos(line, 0, 0);
        lv_obj_set_style_line_width(line, 3, 0);
        lv_obj_set_style_line_color(line, lv_color_black(), 0);
        lv_line_set_points(line, spark_points.data(), spark_points.size());
    } else {
        double maximum = 0;
        bool any_known = false;
        for (const auto &b : u.buckets)
            if (MetricKnown(b.counts, metric)) {
                any_known = true;
                maximum = std::max(maximum, metric == 1   ? b.counts.cost
                                        : metric == 2 ? double(b.counts.requests)
                                                      : double(b.counts.Total()));
            }
        if (!any_known || maximum <= 0) {
            Label(plot, any_known ? "此范围暂无用量" : "未提供此指标数据", 20, 35, width - 40);
            return;
        }
        const int bw = std::max(2, int(width / n * .65));
        for (size_t i = 0; i < n; i++) {
            const auto &c = u.buckets[i].counts;
            if (!MetricKnown(c, metric))
                continue;
            const int bx = int(i * width / n) + (int(width / n) - bw) / 2;
            int bottom = graph_h;
            if (metric == 0) {
                const uint64_t values[] = {c.input, c.output, c.write, c.read};
                double accumulated = 0;
                for (int k = 0; k < 4; k++) {
                    accumulated += values[k];
                    const int edge = graph_h - int(std::round(accumulated / maximum * (graph_h - 10)));
                    const int segment = bottom - edge;
                    if (segment > 0) Pattern(plot, bx, edge, bw, segment, k);
                    bottom = edge;
                }
            } else {
                const auto value = metric == 1 ? c.cost : double(c.requests);
                const int h = int(value / maximum * (graph_h - 10));
                if (h <= 0)
                    continue;
                auto *r = Box(plot, bx, graph_h - h, bw, h);
                lv_obj_set_style_bg_color(r, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
            }
        }
    }
    Label(plot, u.range == Range::Today ? "00:00" : u.first.empty() ? "" : u.first.substr(5), 0,
          graph_h + 8, 150, Latin());
    auto *l = Label(plot, u.range == Range::Today ? Clock(u.received_at) : u.last.substr(5), width - 150,
                    graph_h + 8, 150, Latin());
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
}
void RangeButtons(const View &v, int x) {
    const char *names[] = {"今日", "7天", "30天"};
    for (int i = 0; i < 3; ++i)
        Button(body, names[i], x + i * 112, 0, 108, 84, ChangeRange, i,
               i == int(v.usage.range), true);
}
void Overview(const View &v, int w, int h) {
    Title("消耗总览", "");
    RangeButtons(v, w - 332);
    Button(body, (v.user_id > 0 ? UserName(v) : "选择用户") + " ▾", w - 592, 0, 244, 84, ShowUsers, 0, false, true);
    const int top = 96, content = h - top;
    const int left = int((w - 26) * 1.08 / 2.08), right = left + 26, rw = w - right;
    Label(body, RangeTitle(v.usage.range) + " Token 总消耗", 0, top, left);
    std::string number = v.usage.valid && v.usage.totals.tokens_known ? Compact(v.usage.totals.Total()) : "--", unit;
    if (!number.empty() && (number.back() == 'M' || number.back() == 'K')) {
        unit = number.back(); number.pop_back();
    }
    const auto *hero = number.size() > 8 ? NumberFont() : &inkmeter_digits_font;
    auto *value = Label(body, number, 0, top + 16, left - 75, hero);
    lv_obj_set_width(value, LV_SIZE_CONTENT);
    lv_point_t size; lv_text_get_size(&size, number.c_str(), hero, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    Label(body, unit, size.x + 12, top + 16 + hero->line_height - hero->base_line - NumberFont()->line_height + NumberFont()->base_line, 70, NumberFont());
    Label(body, "实际费用 · USD", 0, top + 152, left / 2);
    Label(body, "请求次数", left / 2, top + 152, left / 2);
    Label(body, v.usage.valid && v.usage.totals.cost_known ? Money(v.usage.totals.cost) : "—", 0, top + 188, left / 2, NumberFont());
    const auto requests = v.usage.valid && v.usage.totals.requests_known ? Integer(v.usage.totals.requests) : "—";
    lv_point_t request_size; lv_text_get_size(&request_size, requests.c_str(), NumberFont(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    Label(body, requests, left / 2, top + 188, left / 2, request_size.x > left / 2 ? Small() : NumberFont());
    Plot(body, v.usage, 0, top + 253, left, std::max(100, content - 253), true);
    Divider(body, right, top, content);
    auto *token = Box(body, right + 32, top, rw - 32, content);
    const int tw = rw - 32, cellw = (tw - 20) / 2, grid_y = 47, hit_y = content - 77;
    const int cellh = (hit_y - grid_y) / 2;
    Label(token, "Token 构成", 0, 0, tw, Normal());
    const char *names[] = {"普通输入", "模型输出", "缓存写入", "缓存读取"};
    const auto &c = v.usage.totals;
    const uint64_t values[] = {c.input, c.output, c.write, c.read};
    for (int i = 0; i < 4; ++i) {
        const int x = (i % 2) * (cellw + 20), y = grid_y + (i / 2) * cellh;
        Label(token, names[i], x, y + 18, cellw);
        std::string n = v.usage.valid && c.tokens_known ? Compact(values[i]) : "—", suffix;
        if (!n.empty() && (n.back() == 'K' || n.back() == 'M')) {
            suffix = n.back(); n.pop_back();
            if (n.find('.') != std::string::npos) {
                while (!n.empty() && n.back() == '0') n.pop_back();
                if (!n.empty() && n.back() == '.') n.pop_back();
            }
        }
        const auto *font = n == "—" ? Small() : TokenFont();
        lv_point_t ns; lv_text_get_size(&ns, n.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        auto *label = Label(token, n, x, y + 50, cellw, font); lv_obj_set_width(label, std::min(cellw, int(ns.x)));
        if (!suffix.empty())
            Label(token, suffix, x + ns.x + 7, y + 50 + font->line_height - font->base_line - Small()->line_height + Small()->base_line, 45, Small());
    }
    Rule(token, 0, hit_y, tw);
    Label(token, "缓存命中率", 0, hit_y + 27, tw - 140);
    const double hit = v.usage.valid ? c.HitRate() : -1;
    char value_text[32]; if (hit >= 0) std::snprintf(value_text, sizeof(value_text), "%.1f%%", hit); else std::snprintf(value_text, sizeof(value_text), "—");
    auto *hit_label = Label(token, value_text, tw - 140, hit_y + 22, 140, Normal());
    lv_obj_set_style_text_align(hit_label, LV_TEXT_ALIGN_RIGHT, 0);
    Bar(token, 0, content - 10, tw, 10, std::max(0.0, hit));
}
void ShowMetrics(lv_event_t *) {
    HideDialog();
    dialog = Box(screen, 0, 0, DisplayUiLogW(), DisplayUiLogH());
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
    lv_obj_add_flag(dialog, LV_OBJ_FLAG_CLICKABLE);
    const int x = std::max(24, (DisplayUiLogW() - 852) / 2), width = DisplayUiLogW() - 2 * x;
    Label(dialog, "分析指标", x, 38, width - 120, Normal());
    Button(dialog, "关闭", x + width - 104, 20, 104, 84, [](lv_event_t *) { HideDialog(); });
    Rule(dialog, x, 116, width);
    const char *names[] = {"Token", "费用", "请求"};
    for (int i = 0; i < 3; ++i)
        Button(dialog, names[i], x, 132 + i * 100, width, 88, [](lv_event_t *e) { Metric(e); HideDialog(); }, i, metric == i);
}
void Analysis(const View &v, int w, int h) {
    Title("用量分析", "");
    RangeButtons(v, w - 528);
    Button(body, std::string(metric == 0 ? "Token" : metric == 1 ? "费用" : "请求") + " ▾", w - 180, 0, 180, 84, ShowMetrics);
    const int top = 96, height = h - top, left = int((w - 24) * 1.22 / 2.22), right = left + 24, mw = w - right - 28;
    auto *chart = Box(body, 0, top, left, height);
    auto *user = Label(chart, UserName(v) + " · UTC+8", 0, 0, left - 185);
    lv_obj_set_height(user, 35); lv_label_set_long_mode(user, LV_LABEL_LONG_DOT);
    auto *total = Label(chart, MetricTotal(v.usage), left - 185, 0, 185);
    lv_obj_set_style_text_align(total, LV_TEXT_ALIGN_RIGHT, 0);
    Plot(chart, v.usage, 0, 35, left, std::max(110, height - 122), false);
    if (metric == 0) {
        const char *names[] = {"普通输入", "模型输出", "缓存写入", "缓存读取"};
        for (int i = 0; i < 4; ++i) {
            const int x = (i % 2) * (left / 2), y = height - 75 + (i / 2) * 39;
            Pattern(chart, x, y + 6, 20, 20, i); Label(chart, names[i], x + 30, y, left / 2 - 30);
        }
    } else Label(chart, metric == 1 ? "实际费用 · USD" : "请求次数", 0, height - 50, left);
    Divider(body, right, top, height);
    auto *panel = Box(body, right + 28, top, mw, height);
    Label(panel, "模型占比", 0, 28, mw - 140, Normal());
    Button(panel, sort_cost ? "费用 ↓" : "Token ↓", mw - 130, 0, 130, 84, Sort, 0, false, true);
    model_list = Box(panel, 0, 94, mw, height - 94);
    MakeScrollable(model_list);
    auto models = v.usage.models;
    std::sort(models.begin(), models.end(), [](const Model &a, const Model &b) {
        const bool ka = sort_cost ? a.cost_known : a.tokens_known, kb = sort_cost ? b.cost_known : b.tokens_known;
        return ka != kb ? ka : sort_cost ? a.cost > b.cost : a.tokens > b.tokens;
    });
    const double denominator = sort_cost ? v.usage.totals.cost : double(v.usage.totals.Total());
    int y = 0;
    for (const auto &m : models) {
        const int name_height = TextHeight(m.id, mw - 10, Small(), 4);
        const int rowh = std::max(116, name_height + 93);
        auto *row = Box(model_list, 0, y, mw - 8, rowh);
        auto *model_name = Label(row, m.id, 0, 18, mw - 10);
        lv_obj_set_style_text_line_space(model_name, 4, 0);
        const bool known = sort_cost ? m.cost_known : m.tokens_known;
        const bool share_known = known && (sort_cost ? v.usage.totals.cost_known : v.usage.totals.tokens_known) && denominator > 0;
        char percent[32]; if (share_known) std::snprintf(percent, sizeof(percent), "%.1f%%", (sort_cost ? m.cost : double(m.tokens)) * 100 / denominator); else std::snprintf(percent, sizeof(percent), "—");
        Label(row, !known ? "—" : sort_cost ? "USD " + Money(m.cost) : Compact(m.tokens), 0, name_height + 32, mw - 125);
        auto *p = Label(row, percent, mw - 120, name_height + 32, 110); lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_RIGHT, 0);
        if (share_known) Bar(row, 0, rowh - 23, mw - 10, 7, (sort_cost ? m.cost : double(m.tokens)) * 100 / denominator);
        y += rowh;
    }
    if (models.empty()) Label(model_list, v.usage.valid ? "此范围暂无模型记录" : "等待用量同步", 0, 20, mw);
    lv_obj_scroll_to_y(model_list, model_scroll, LV_ANIM_OFF);
}
void QuotaSync(lv_event_t *e) {
    SyncAccount(*static_cast<int64_t *>(lv_event_get_user_data(e)));
}
std::string WindowTitle(const Window &w) { return w.label == "5h" ? "5小时已用" : w.label == "7d" ? "7天已用" : w.label; }
std::string ResetText(const Window&w){return !w.known?"未提供此窗口":w.reset_at?Clock(w.reset_at,true)+" 重置":w.reset_text.empty()?"未提供重置时间":w.reset_text;}
int WindowHeight(const Window&w,int width){return std::max(20,TextHeight(WindowTitle(w),width-100,Latin()))+5+(w.known?7:0)+6+TextHeight(ResetText(w),width,Latin())+10;}
int BalanceHeight(const Balance&b,int width){return std::max(TextHeight(b.label,width-180,Latin()),TextHeight(b.value,175,b.value.size()>10?Latin():&lv_font_montserrat_28))+10;}
int WindowRows(lv_obj_t *parent, const Account &a, int y, int width) {
    for (const auto &w : a.windows) {
        Label(parent, WindowTitle(w), 0, y, width - 100, Latin());
        char p[32];
        if (w.known)
            std::snprintf(p, sizeof(p), "%.1f%%", w.used);
        else
            std::snprintf(p, sizeof(p), "—");
        auto *l = Label(parent, p, width - 95, y, 95, Latin());
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        const int line=std::max(20,TextHeight(WindowTitle(w),width-100,Latin()));
        if (w.known)Bar(parent, 0, y + line+5, width, 7, w.used);
        Label(parent,ResetText(w),0,y+line+5+(w.known?7:0)+6,width,Latin());
        y += WindowHeight(w,width);
    }
    for (const auto &b : a.balances) {
        Label(parent, b.label, 0, y, width - 180, Latin());
        auto *l = Label(parent, b.value, width - 175, y, 175, b.value.size()>10?Latin():&lv_font_montserrat_28);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        y += BalanceHeight(b,width);
    }
    return y;
}
void QuotaGroup(lv_event_t*e){quota_group=static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));quota_scroll=0;if(quota_list)lv_obj_scroll_to_y(quota_list,0,LV_ANIM_OFF);ScheduleRender();}
void Quotas(const View &v, int w, int h) {
    Title("账户额度", "");
    const int groups = std::max(1, int((v.accounts.size() + 7) / 8));
    quota_group = std::max(0, std::min(groups - 1, quota_group));
    if (groups > 1) {
        Label(body, std::to_string(v.selected_accounts) + " 个账户", w - 540, 27, 180);
        auto *prev = Button(body, "前一组", w - 350, 0, 112, 84, QuotaGroup, quota_group - 1, false, true);
        Label(body, std::to_string(quota_group + 1) + "/" + std::to_string(groups), w - 222, 27, 80);
        auto *next = Button(body, "后一组", w - 140, 0, 140, 84, QuotaGroup, quota_group + 1, false, true);
        if (!quota_group) lv_obj_add_state(prev, LV_STATE_DISABLED);
        if (quota_group == groups - 1) lv_obj_add_state(next, LV_STATE_DISABLED);
    } else {
        auto *count = Label(body, std::to_string(v.selected_accounts) + " 个账户 · 上次快照", w - 440, 27, 440);
        lv_obj_set_style_text_align(count, LV_TEXT_ALIGN_RIGHT, 0);
    }
    quota_list = Box(body, 0, 96, w, h - 96);
    MakeScrollable(quota_list);
    const int cw = (w - 28) / 2;
    int row_y = 0, row_h = 0;
    // shortcut: eight cards per group bound the live LVGL object count.
    const size_t first = quota_group * 8, last = std::min(v.accounts.size(), first + 8);
    for (size_t i = first; i < last; ++i) {
        const auto &a = v.accounts[i];
        const int col = (i - first) % 2, inset = col ? 28 : 16, iw = cw - inset - 16;
        int data_height = 0;
        for (const auto &window : a.windows) data_height += WindowHeight(window, iw);
        for (const auto &balance : a.balances) data_height += BalanceHeight(balance, iw);
        const auto note = a.note.empty() ? std::string("尚未采集") : a.note;
        if (!a.snapshot) data_height += TextHeight(note, iw, Small()) + 20;
        const int ch = std::max(370, 66 + data_height + 106);
        if (!col) { row_y += i > first ? row_h + 20 : 0; row_h = 0; }
        row_h = std::max(row_h, ch);
        auto *card = Box(quota_list, col * (cw + 20), row_y, cw, ch);
        if (col) Divider(card, 0, 0, ch);
        if (i >= first + 2) Rule(card, 0, 0, cw);
        const bool urgent = std::any_of(a.windows.begin(), a.windows.end(), [](const Window &window) { return window.known && window.used >= 90; });
        auto *name = Label(card, a.name, inset, 10, iw - (urgent ? 145 : 0), Normal());
        lv_obj_set_height(name, 40); lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        if (urgent) {
            auto *badge = Box(card, cw - 150, 12, 134, 34);
            lv_obj_set_style_bg_color(badge, lv_color_black(), 0); lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            auto *label = Label(badge, "接近上限", 8, 1, 118); lv_obj_set_style_text_color(label, lv_color_white(), 0);
        }
        auto *contents = Box(card, inset, 62, iw, ch - 168);
        const int bottom = WindowRows(contents, a, 0, iw);
        if (!a.snapshot) Label(contents, note, 0, bottom + 12, iw);
        Rule(card, inset, ch - 100, iw);
        const std::string stamp = a.error.empty() ? a.observed_at ? "采集 " + Clock(a.observed_at) : a.snapshot ? "取回 " + Clock(a.received_at) : "暂无快照" : a.error;
        auto *time = Label(card, stamp, inset, ch - 79, iw - 200); lv_obj_set_height(time, 31); lv_label_set_long_mode(time, LV_LABEL_LONG_DOT);
        auto *source = Label(card, a.platform == "grok" ? "可发出短测试请求" : a.source, inset, ch - 43, iw - 200); lv_obj_set_height(source, 31); lv_label_set_long_mode(source, LV_LABEL_LONG_DOT);
        auto *button = IdButton(card, !a.manual_supported ? "暂不支持" : v.syncing_account == a.id ? "同步中…" : a.platform == "gemini" && !a.ollama && !a.opencode ? "读取本地估算" : "同步上游额度", cw - 202, ch - 94, 186, 84, QuotaSync, a.id);
        if (!a.manual_supported || v.busy) lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    if (v.accounts.empty()) Label(quota_list, v.busy ? "正在读取站点已有快照…" : "请在手机上选择额度账户并保存", 20, 90, w - 40);
    lv_obj_scroll_to_y(quota_list, quota_scroll, LV_ANIM_OFF);
}
void OpenConfig(lv_event_t *) {
    OpenConfiguration();
    ScheduleRender();
}
void CloseConfig(lv_event_t *) {
    CloseConfiguration();
    ScheduleRender();
}
void Settings(const View &v, int w, int h) {
    Title("设备设置", "");
    auto *period = Label(body, "用量每 " + std::to_string(v.usage_minutes) + " 分钟更新", w - 430, 27, 430);
    lv_obj_set_style_text_align(period, LV_TEXT_ALIGN_RIGHT, 0);
    const int top = 96, height = h - top, left = int((w - 26) / 2.07), right = left + 26, sw = w - right;
    Divider(body, left - 1, top, height);
    std::string site = v.site;
    if (site.rfind("https://", 0) == 0) site.erase(0, 8);
    const std::pair<std::string, std::string> rows[] = {
        {"网络", v.network_status.empty() ? "请在原厂设置联网" : v.network_status},
        {"站点", site.empty() ? "未配置" : site},
        {"用户", v.user_id > 0 ? UserName(v) : "未选择"},
        {"额度账户", std::to_string(v.selected_accounts) + " 个"},
        {"最后成功", Clock(v.usage.received_at, true)}, {"时区", "UTC+8"}};
    int y = top;
    for (int i = 0; i < 6; ++i) {
        Label(body, rows[i].first, 0, y, 150);
        auto *value = Label(body, rows[i].second, 160, y, left - 192);
        lv_obj_set_height(value, 32); lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
        y += i == 0 ? 47 : 59;
    }
    auto *setup = Box(body, right, top, sw, height);
    const auto pairing = GetPairingInfo();
    if (pairing.open) {
        auto *quiet = Box(setup, 0, 0, 224, 224);
        lv_obj_set_style_bg_opa(quiet, LV_OPA_COVER, 0);
        auto *qr = lv_qrcode_create(quiet); lv_qrcode_set_size(qr, 184); lv_obj_set_pos(qr, 20, 20);
        lv_qrcode_update(qr, pairing.url.data(), pairing.url.size());
        Label(setup, "扫码配置", 246, 15, sw - 246, Normal());
        Label(setup, pairing.paired ? "手机已配对" : "一次性配对码", 246, 62, sw - 246);
        Label(setup, pairing.paired ? "OK" : pairing.code, 246, 105, sw - 246, NumberFont());
        Label(setup, pairing.url, 246, 171, sw - 246);
        Label(setup, "HTTP 明文传输，仅限可信局域网。\n同网设备可能读取 Key 与会话。", 0, 230, sw);
        Label(setup, "空闲 " + std::to_string((pairing.remaining_seconds + 59) / 60) + " 分钟关闭", 0, height - 58, sw - 180);
        Button(setup, "关闭配置", sw - 168, height - 84, 168, 84, CloseConfig);
    } else {
        Label(setup, "手机配置", 0, 47, sw, Normal());
        auto *hint = Label(setup, pairing.starting ? "正在开启HTTP配置服务…" : pairing.error.empty() ? "手机与设备连接同一 Wi-Fi" : pairing.error, 0, 91, sw);
        lv_obj_set_height(hint, 60); lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
        auto *open = Button(setup, pairing.starting ? "正在开启…" : "开启配置模式", 0, 154, sw, 88, OpenConfig, 0, true);
        if (pairing.starting) lv_obj_add_state(open, LV_STATE_DISABLED);
        Label(setup, "HTTP 明文传输，仅限可信局域网。\n空闲 10 分钟、退出或待机后关闭。", 0, 267, sw);
    }
}
void Render() {
    if (!loaded || sleeping || scrolling || !screen || !lv_obj_is_valid(screen))
        return;
    repaint_pending = false;
    // Read the revision first so a worker update during GetView requests another paint.
    const uint64_t rendering_revision = Revision();
    if (model_list && lv_obj_is_valid(model_list))
        model_scroll = lv_obj_get_scroll_y(model_list);
    if (quota_list && lv_obj_is_valid(quota_list))
        quota_scroll = lv_obj_get_scroll_y(quota_list);
    if (dialog) {
        const auto v = GetView();
        if (!v.busy) {
            HideDialog();
            ShowUsers(nullptr);
        }
        seen_revision = rendering_revision;
        return;
    }
    model_list = nullptr;
    quota_list = nullptr;
    lv_obj_clean(body);
    spark_points.clear();
    const auto v = GetView();
    seen_revision = rendering_revision;
    const std::string stamp = v.busy ? "同步中…" : !v.configured ? "未配置" : !v.status.empty() ? v.status : v.usage.valid ? "已更新 " + Clock(v.usage.received_at) : "等待同步";
    lv_label_set_text(status_label, stamp.c_str());
    const int w = DisplayUiLogW() - 56, h = DisplayUiLogH() - 208;
    if (page == 0)
        Overview(v, w, h);
    else if (page == 1)
        Analysis(v, w, h);
    else if (page == 2)
        Quotas(v, w, h);
    else
        Settings(v, w, h);

    const char *names[] = {"总览", "分析", "额度", "设置"};
    lv_obj_clean(footer);
    Rule(footer, 0, 0, w, 55);
    const int cw = w / 4;
    for (int i = 0; i < 4; i++)
        Button(footer, names[i], i * cw, 6, cw - 12, 84, Page, i, i == page, true);
}
void Poll(lv_timer_t *) {
    if (!loaded || sleeping)
        return;
    Tick();
    const auto p = GetPairingInfo();
    const std::string marker =
        std::to_string(p.open) + std::to_string(p.paired) + std::to_string(p.starting) + p.error;
    const int minute = p.remaining_seconds / 60;
    if (Revision() != seen_revision ||
        (page == 3 && (marker != seen_pair_state || minute != seen_config_minute))) {
        seen_pair_state = marker;
        seen_config_minute = minute;
        ScheduleRender();
    }
}
bool Key(const char *name) {
    if (!name)
        return false;
    if (sleeping)
        return true;
    if (std::strcmp(name, "vk_home") == 0) {
        HideDialog();
        Leave();
        ScreenGoHome();
        return true;
    }
    if (dialog) {
        if (std::strcmp(name, "vk_prev") == 0 || std::strcmp(name, "vk_next") == 0) {
            const auto v = GetView();
            BrowseUsers(std::max(1, v.user_page + (std::strcmp(name, "vk_next") == 0 ? 1 : -1)));
        }
        return true;
    }
    if (std::strcmp(name, "vk_prev") == 0 || std::strcmp(name, "vk_next") == 0) {
        SelectPage(page + (std::strcmp(name, "vk_next") == 0 ? 1 : -1));
        return true;
    }
    return false;
}
void Lifecycle(lv_event_t *e) {
    const auto code = lv_event_get_code(e);
    if (code == LV_EVENT_SCREEN_LOADED) {
        loaded = true;
        sleeping = false;
        stay_awake.emplace(PowerNeed::StandbyInhibit);
        Enter();
        ScheduleRender();
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        if(model_list&&lv_obj_is_valid(model_list))model_scroll=lv_obj_get_scroll_y(model_list);
        if(quota_list&&lv_obj_is_valid(quota_list))quota_scroll=lv_obj_get_scroll_y(quota_list);
        scrolling=false;
        repaint_pending=false;
        EpdSetUiScrolling(false);
        loaded = false;
        sleeping = false;
        Leave();
        stay_awake.reset();
        if (timer) {
            lv_timer_delete(timer);
            timer = nullptr;
        }
        ScreenPaintCoalesceReset(&paint);
    } else if (code == LV_EVENT_DELETE) {
        if (lv_event_get_target(e) == screen) {
            screen = nullptr;
            body = nullptr;
            footer = nullptr;
            dialog = nullptr;
            model_list = nullptr;
            quota_list = nullptr;
            spark_points.clear();
        }
    }
}
} // namespace

lv_obj_t *InkMeterScreen::Create() {
    using namespace inkmeter;
    ScreenSetIsHome(false);
    DisplayUiSetFullScreen(false);
    DisplayUiSetOrient(kDisplayUiLandLeft);
    model_list=nullptr;
    quota_list=nullptr;
    if(auto* display=LVAdapterDisplay::Instance())display->BindStatusWidgets(nullptr,nullptr,nullptr,nullptr,nullptr,nullptr);
    screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, DisplayUiLogW(), DisplayUiLogH());
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, Normal(), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    Label(screen, "inkmeter.", 28, 25, 260, Normal());
    status_label = Label(screen, "未配置", DisplayUiLogW() - 570, 29, 380);
    lv_obj_set_height(status_label, 32); lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_RIGHT, 0);
    Button(screen, "刷新", DisplayUiLogW() - 140, 2, 112, 84, Refresh);
    Rule(screen, 28, 87, DisplayUiLogW() - 56);
    body = Box(screen, 28, 100, DisplayUiLogW() - 56, DisplayUiLogH() - 208);
    footer = Box(screen, 28, DisplayUiLogH() - 96, DisplayUiLogW() - 56, 96);
    paint.paint = Render;
    paint.defer_while_epd_busy = true;
    timer = lv_timer_create(Poll, 1000, nullptr);
    lv_obj_add_event_cb(screen, Lifecycle, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(screen, Lifecycle, LV_EVENT_SCREEN_UNLOADED, nullptr);
    lv_obj_add_event_cb(screen, Lifecycle, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(screen, Gesture, LV_EVENT_GESTURE, nullptr);
    VkKey_AttachScreen(screen, "inkmeter", VkKeyScreenDesc{Create, Key});
    return screen;
}
bool InkMeterScreen::IsActive() {
    return loaded && screen != nullptr;
}
void InkMeterScreen::SetStandby(bool value) {
    if (!IsActive())
        return;
    sleeping = value;
    inkmeter::SetStandby(value);
    if (value) {
        scrolling = false;
        EpdSetUiScrolling(false);
        stay_awake.reset();
    } else {
        DisplayUiSetFullScreen(false);
        DisplayUiSetOrient(kDisplayUiLandLeft);
        stay_awake.emplace(PowerNeed::StandbyInhibit);
        seen_revision = 0;
        ScheduleRender();
    }
}
