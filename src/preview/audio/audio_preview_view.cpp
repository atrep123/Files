#include "core/files_language.hpp"
#ifdef FILES_LAYOUT_AUDIT
#include "layout_audit/layout_audit.hpp"
#endif

#include "preview/audio/audio_preview_view.hpp"

#include "assets/assets.h"
#include "assets/font_assets.hpp"
#include "views/ui_style.hpp"
#include <core/color/color.hpp>
#include <core/easing/ease.hpp>
#include <miniaudio.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace files {
namespace {

constexpr int32_t kTitleWidth              = 252;
constexpr int32_t kTitleY                  = 2;
constexpr int32_t kWaveformWidth           = 280;
constexpr int32_t kWaveformHeight          = 60;
constexpr int32_t kWaveformX               = 0;
constexpr int32_t kWaveformY               = -23;
constexpr int32_t kWaveformBarWidth        = 1;
constexpr int32_t kWaveformBarPitch        = 3;
constexpr int32_t kWaveformMinBarHeight    = 2;
constexpr int32_t kWaveformMaxBarHeight    = 56;
constexpr size_t kWaveformVisibleBarCount  = 94;
constexpr uint32_t kWaveformIdleColor      = 0xFFFFFF;
constexpr uint32_t kWaveformPlayingColor   = ui::success;
constexpr float kWaveformColorDuration     = 0.4f;
constexpr ma_uint32 kWaveformSampleRate    = 48000;
constexpr ma_uint32 kWaveformChannels      = 1;
constexpr ma_format kWaveformFormat        = ma_format_f32;
constexpr float kWaveformBarsPerSecond     = 24.0f;
constexpr size_t kWaveformMaxDecodedBars   = 8192;
constexpr int32_t kPlayheadWidth           = 2;
constexpr int32_t kPlayheadHeight          = 64;
constexpr uint32_t kPlayheadColor          = ui::accent;
constexpr int32_t kProgressBarX            = 20;
constexpr int32_t kProgressBarY            = 106;
constexpr int32_t kProgressBarWidth        = 280;
constexpr int32_t kProgressBarHeight       = 5;
constexpr int32_t kProgressBarRadius       = 2;
constexpr uint32_t kProgressTrackColor     = ui::black;
constexpr uint32_t kProgressFillColor      = ui::accent;
// Keep both clocks above the one-row connection notice.
constexpr int32_t kTimeLabelY              = 111;
constexpr int32_t kTimeLabelWidth          = 96;
constexpr uint32_t kTimeTextColor          = ui::secondary;
std::string fileTitle(const AudioPreviewFile& file)
{
    return file.entry.name.empty() ? file.entry.path : file.entry.name;
}

std::string timeText(float seconds)
{
    int total = std::max(0, static_cast<int>(std::round(seconds)));
    int hours = total / 3600;
    int mins  = total / 60 % 60;
    int secs  = total % 60;

    char buffer[16] = {};
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hours, mins, secs);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d", mins, secs);
    }
    return buffer;
}

std::string remainingTimeText(float durationSec, float progressSec)
{
    const auto time = timeText(durationSec - progressSec);
    return time == "0:00" ? time : "-" + time;
}

const void* speedIcon(float speed)
{
    if (speed >= 5.0f) {
        return &image_icon_playback_5x;
    }
    if (speed >= 2.0f) {
        return &image_icon_playback_2x;
    }
    return &image_icon_playback_1x;
}

}  // namespace

class AudioPreviewView::WaveformView {
public:
    explicit WaveformView(lv_obj_t* parent)
        : _panel(std::make_unique<smooth_ui_toolkit::lvgl_cpp::Container>(parent)), _color(kWaveformIdleColor)
    {
        _color.duration       = kWaveformColorDuration;
        _color.easingFunction = smooth_ui_toolkit::ease::ease_out_quad;
        _color.begin();

        _panel->setSize(kWaveformWidth, kWaveformHeight);
        _panel->align(LV_ALIGN_CENTER, kWaveformX, kWaveformY);
        _panel->setBgOpa(LV_OPA_TRANSP);
        _panel->setBorderWidth(0);
        _panel->setShadowWidth(0);
        _panel->setPaddingAll(0);
        _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
        _panel->addEventCb(onDraw, LV_EVENT_DRAW_MAIN, this);
        _bars.assign(kWaveformVisibleBarCount, 0.0f);
    }

    void load(const AudioPreviewFile& file)
    {
        _duration_sec = static_cast<float>(file.durationSec);
        _progress_sec = 0.0f;
        decode(file.entry.path, file.durationSec);
        lv_obj_invalidate(_panel->raw_ptr());
    }

    void setProgress(float progressSec)
    {
        _progress_sec = progressSec;
        lv_obj_invalidate(_panel->raw_ptr());
    }

    void setPlaying(bool playing)
    {
        _color.move(playing ? kWaveformPlayingColor : kWaveformIdleColor);
    }

    void tick()
    {
        _color.update();
        lv_obj_invalidate(_panel->raw_ptr());
    }

private:
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _panel;
    smooth_ui_toolkit::color::AnimateRgb_t _color;
    std::vector<float> _bars;
    float _duration_sec = 0.0f;
    float _progress_sec = 0.0f;

    lv_color_t currentColor() const
    {
        return lv_color_hex(_color.toHex());
    }

    void decode(const std::string& path, uint32_t durationSec)
    {
        _bars.assign(kWaveformVisibleBarCount, 0.0f);

        ma_decoder decoder{};
        ma_decoder_config config = ma_decoder_config_init(kWaveformFormat, kWaveformChannels, kWaveformSampleRate);
        ma_result result         = ma_decoder_init_file(path.c_str(), &config, &decoder);
        if (result != MA_SUCCESS) {
            return;
        }

        ma_uint64 total_frames = 0;
        result                 = ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);
        if (result != MA_SUCCESS || total_frames == 0) {
            total_frames = static_cast<ma_uint64>(durationSec) * kWaveformSampleRate;
        }

        const size_t bar_count =
            std::clamp(static_cast<size_t>(std::ceil(std::max(1.0f, _duration_sec) * kWaveformBarsPerSecond)),
                       kWaveformVisibleBarCount, kWaveformMaxDecodedBars);

        std::vector<float> bars(bar_count, 0.0f);
        std::vector<float> buffer(1024, 0.0f);
        ma_uint64 cursor = 0;
        while (cursor < total_frames) {
            ma_uint64 frames_read = 0;
            ma_decoder_read_pcm_frames(&decoder, buffer.data(), buffer.size(), &frames_read);
            if (frames_read == 0) {
                break;
            }

            for (ma_uint64 i = 0; i < frames_read; ++i) {
                size_t index = static_cast<size_t>((cursor + i) * static_cast<ma_uint64>(bar_count) / total_frames);
                if (index >= bars.size()) {
                    index = bars.size() - 1;
                }
                bars[index] = std::max(bars[index], std::abs(buffer[static_cast<size_t>(i)]));
            }
            cursor += frames_read;
        }
        ma_decoder_uninit(&decoder);

        float max_amp = 0.0f;
        for (float value : bars) {
            max_amp = std::max(max_amp, value);
        }
        if (max_amp > 0.0f) {
            for (auto& value : bars) {
                value = std::pow(std::clamp(value / max_amp, 0.0f, 1.0f), 0.68f);
            }
        }

        _bars = std::move(bars);
    }

    float barValueForVisibleIndex(size_t visibleIndex) const
    {
        if (_bars.empty() || _duration_sec <= 0.0f) {
            return -1.0f;
        }

        const float duration     = std::max(_duration_sec, 0.001f);
        const float progress     = std::clamp(_progress_sec / duration, 0.0f, 1.0f);
        const float center_index = progress * static_cast<float>(_bars.size() - 1);
        const float source_index =
            center_index + static_cast<float>(visibleIndex) - static_cast<float>(kWaveformVisibleBarCount / 2);
        const int index = static_cast<int>(std::round(source_index));
        if (index < 0 || index >= static_cast<int>(_bars.size())) {
            return -1.0f;
        }
        return _bars[static_cast<size_t>(index)];
    }

    void draw(lv_event_t* event)
    {
        lv_layer_t* layer = lv_event_get_layer(event);
        if (!layer) {
            return;
        }

        lv_area_t coords;
        lv_obj_get_coords(_panel->raw_ptr(), &coords);

        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = currentColor();
        line_dsc.width = kWaveformBarWidth;

        const int32_t mid_y = coords.y1 + kWaveformHeight / 2;
        const int32_t start_x =
            coords.x1 + (kWaveformWidth - ((kWaveformVisibleBarCount - 1) * kWaveformBarPitch + 1)) / 2;

        for (size_t i = 0; i < kWaveformVisibleBarCount; ++i) {
            const auto value = barValueForVisibleIndex(i);
            if (value < 0.0f) continue;
            int32_t bar_h = kWaveformMinBarHeight +
                            static_cast<int32_t>(std::round(value *
                                                            (kWaveformMaxBarHeight - kWaveformMinBarHeight)));
            bar_h = std::clamp(bar_h, kWaveformMinBarHeight, kWaveformMaxBarHeight);

            const int32_t x    = start_x + static_cast<int32_t>(i) * kWaveformBarPitch;
            const int32_t half = bar_h / 2;
            line_dsc.p1.x      = x;
            line_dsc.p1.y      = mid_y - half;
            line_dsc.p2.x      = x;
            line_dsc.p2.y      = mid_y + half;
            line_dsc.opa       = LV_OPA_COVER;
            lv_draw_line(layer, &line_dsc);
        }
    }

    static void onDraw(lv_event_t* event)
    {
        auto* self = static_cast<WaveformView*>(lv_event_get_user_data(event));
        if (self) {
            self->draw(event);
        }
    }
};

class AudioPreviewView::ProgressBar {
public:
    explicit ProgressBar(lv_obj_t* parent)
        : _track(std::make_unique<smooth_ui_toolkit::lvgl_cpp::Container>(parent)),
          _fill(std::make_unique<smooth_ui_toolkit::lvgl_cpp::Container>(_track->raw_ptr()))
    {
        _track->setSize(kProgressBarWidth, kProgressBarHeight);
#ifdef FILES_LAYOUT_AUDIT
        lv_obj_add_flag(_track->raw_ptr(), layout_audit::progress_flag);
#endif
        _track->setPos(kProgressBarX, kProgressBarY);
        _track->setBgColor(lv_color_hex(kProgressTrackColor));
        _track->setBgOpa(LV_OPA_COVER);
        _track->setRadius(kProgressBarRadius);
        _track->setBorderWidth(1);
        lv_obj_set_style_border_color(_track->raw_ptr(), lv_color_hex(ui::secondary), 0);
        _track->setShadowWidth(0);
        _track->setPaddingAll(0);
        _track->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

        _fill->setSize(0, kProgressBarHeight - 2);
        _fill->setPos(0, 0);
        _fill->setBgColor(lv_color_hex(kProgressFillColor));
        _fill->setBgOpa(LV_OPA_COVER);
        _fill->setRadius(0);
        _fill->setBorderWidth(0);
        _fill->setShadowWidth(0);
        _fill->setPaddingAll(0);
        _fill->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    }

    void setProgress(float progressSec, float durationSec)
    {
        const float ratio = durationSec <= 0.0f ? 0.0f : std::clamp(progressSec / durationSec, 0.0f, 1.0f);
        _fill->setWidth(static_cast<int32_t>(std::round(ratio * (kProgressBarWidth - 2))));
    }

private:
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _track;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _fill;
};

AudioPreviewView::AudioPreviewView(AudioPreviewViewModel& viewModel) : _view_model(viewModel)
{
}

AudioPreviewView::~AudioPreviewView()
{
    detach();
}

void AudioPreviewView::attach(lv_obj_t* parent)
{
    detach();

    _root = std::make_unique<smooth_ui_toolkit::lvgl_cpp::Container>(parent);
    _root->setSize(lv_pct(100), lv_pct(100));
    _root->setBgColor(lv_color_hex(0x000000));
    _root->setBgOpa(LV_OPA_COVER);
    _root->setBorderWidth(0);
    _root->setPaddingAll(0);
    _root->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
    _root->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    ui::box(_root->raw_ptr(), 0, 0, 320, 20, ui::bar);
    _title_label = std::make_unique<smooth_ui_toolkit::lvgl_cpp::Label>(_root->raw_ptr());
    _title_label->setTextFont(uiFont14());
    _title_label->setTextColor(lv_color_hex(ui::white));
    _title_label->setTextAlign(LV_TEXT_ALIGN_LEFT);
    _title_label->setLongMode(LV_LABEL_LONG_MODE_CLIP);
    _title_label->setSize(kTitleWidth, LV_SIZE_CONTENT);
    _title_label->setPos(8, kTitleY);
    _speed_label = ui::label(_root->raw_ptr(), 268, 2, 44, uiFont12(), ui::secondary);
    lv_obj_set_style_text_align(_speed_label, LV_TEXT_ALIGN_RIGHT, 0);
    _state_label = ui::label(_root->raw_ptr(), 202, 2, 85, uiFont12(), ui::secondary);
    lv_obj_set_style_text_align(_state_label, LV_TEXT_ALIGN_RIGHT, 0);

    _waveform = std::make_unique<WaveformView>(_root->raw_ptr());

    _playhead = std::make_unique<smooth_ui_toolkit::lvgl_cpp::Container>(_root->raw_ptr());
    _playhead->setSize(kPlayheadWidth, kPlayheadHeight);
    _playhead->align(LV_ALIGN_CENTER, 0, kWaveformY);
    _playhead->setBgColor(lv_color_hex(kPlayheadColor));
    _playhead->setBgOpa(LV_OPA_COVER);
    _playhead->setRadius(1);
    _playhead->setBorderWidth(0);
    _playhead->setShadowWidth(0);
    _playhead->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _progress_bar = std::make_unique<ProgressBar>(_root->raw_ptr());

    _elapsed_label = std::make_unique<smooth_ui_toolkit::lvgl_cpp::Label>(_root->raw_ptr());
    _elapsed_label->setTextFont(uiMonoFont12());
    _elapsed_label->setTextColor(lv_color_hex(kTimeTextColor));
    _elapsed_label->setTextAlign(LV_TEXT_ALIGN_LEFT);
    _elapsed_label->setSize(kTimeLabelWidth, LV_SIZE_CONTENT);
    _elapsed_label->setPos(kProgressBarX, kTimeLabelY);

    _remaining_label = std::make_unique<smooth_ui_toolkit::lvgl_cpp::Label>(_root->raw_ptr());
    _remaining_label->setTextFont(uiMonoFont12());
    _remaining_label->setTextColor(lv_color_hex(kTimeTextColor));
    _remaining_label->setTextAlign(LV_TEXT_ALIGN_RIGHT);
    _remaining_label->setSize(kTimeLabelWidth, LV_SIZE_CONTENT);
    _remaining_label->setPos(kProgressBarX + kProgressBarWidth - kTimeLabelWidth, kTimeLabelY);

    _error_panel = ui::box(_root->raw_ptr(), 0, 20, 320, 130, ui::black);
#ifdef FILES_LAYOUT_AUDIT
    lv_obj_add_flag(_error_panel, layout_audit::overlay_flag);
#endif
    const auto* error = local("Cannot play audio", "Zvuk nelze přehrát");
    const int textWidthPx = textWidth(error, uiFont14());
    const int start = (320 - 14 - textWidthPx) / 2;
    auto* icon = ui::label(_error_panel, start, 52, 12, LV_FONT_DEFAULT, ui::error);
    lv_label_set_text(icon, LV_SYMBOL_CLOSE);
    auto* title = ui::label(_error_panel, start + 14, 52, textWidthPx + 2, uiFont14(), ui::error);
    lv_label_set_text(title, error);
    ui::hidden(_error_panel, true);
    _key_bar = std::make_unique<BottomKeyBar>(_root->raw_ptr());

    _view_model.state().observe(this, onStateChanged);
    _view_model.file().observe(this, onFileChanged);
    _view_model.progressSec().observe(this, onProgressChanged);
    _view_model.speed().observe(this, onSpeedChanged);

    renderState(_view_model.state().get());
    renderFile(_view_model.file().get());
    renderProgress(_view_model.progressSec().get());
    renderSpeed(_view_model.speed().get());
}

void AudioPreviewView::detach()
{
    _view_model.speed().removeObserver();
    _view_model.progressSec().removeObserver();
    _view_model.file().removeObserver();
    _view_model.state().removeObserver();

    _key_bar.reset();
    _remaining_label.reset();
    _elapsed_label.reset();
    _progress_bar.reset();
    _playhead.reset();
    _waveform.reset();
    _title_label.reset();
    _state_label = _speed_label = _error_panel = nullptr;
    _root.reset();
}

void AudioPreviewView::tick(uint32_t nowMs)
{
    (void)nowMs;

    if (_waveform) {
        _waveform->tick();
    }
}

void AudioPreviewView::renderState(AudioPlaybackState state)
{
    if (_waveform) {
        _waveform->setPlaying(state == AudioPlaybackState::Playing);
    }
    const bool failed = state == AudioPlaybackState::Error;
    ui::hidden(_error_panel, !failed);
    ui::hidden(_speed_label, failed);
    const bool paused = state == AudioPlaybackState::Paused;
    const bool ended = state == AudioPlaybackState::Stopped;
    const auto* status = paused ? local("Paused", "Pauza") : ended ? local("Ended", "Konec") : "";
    ui::hidden(_state_label, !*status);
    lv_label_set_text(_state_label, status);
    const int width = *status ? 271 - textWidth(status, uiFont12()) : kTitleWidth;
    lv_obj_set_width(_title_label->raw_ptr(), width);
    ui::line(_title_label->raw_ptr(), fileTitle(_view_model.file().get()), width, uiFont14(), std::string::npos, true);
    ui::hidden(_elapsed_label->raw_ptr(), failed);
    ui::hidden(_remaining_label->raw_ptr(), failed);
    updateKeyBar();
}

void AudioPreviewView::renderFile(const AudioPreviewFile& file)
{
    if (_title_label) {
        renderState(_view_model.state().get());
    }
    if (_waveform) {
        _waveform->load(file);
    }
    renderProgress(_view_model.progressSec().get());
}

void AudioPreviewView::renderProgress(float progressSec)
{
    const float duration = static_cast<float>(_view_model.file().get().durationSec);
    if (_waveform) {
        _waveform->setProgress(progressSec);
    }
    if (_progress_bar) {
        _progress_bar->setProgress(progressSec, duration);
    }
    if (_elapsed_label) {
        _elapsed_label->setText(timeText(progressSec));
    }
    if (_remaining_label) {
        _remaining_label->setText(remainingTimeText(duration, progressSec));
    }
}

void AudioPreviewView::renderSpeed(float speed)
{
    if (_speed_label) lv_label_set_text_fmt(_speed_label, "%d×", static_cast<int>(speed));
    updateKeyBar();
}

void AudioPreviewView::updateKeyBar()
{
    if (!_key_bar) {
        return;
    }

    if (_view_model.state().get() == AudioPlaybackState::Error) {
        _key_bar->setItems({});
        return;
    }
    _key_bar->setItems({
        {'5', &image_icon_playback_replay},
        {'6', _view_model.state().get() == AudioPlaybackState::Playing ? &image_icon_playback_pause
                                                                       : &image_icon_playback_play,
         _view_model.state().get() == AudioPlaybackState::Stopped ? local("Replay", "Znovu") : nullptr},
        {'7', &image_icon_playback_forward},
        {'8', speedIcon(_view_model.speed().get())},
    });
}

void AudioPreviewView::onStateChanged(void* context, const AudioPlaybackState& state)
{
    auto* self = static_cast<AudioPreviewView*>(context);
    if (self) {
        self->renderState(state);
    }
}

void AudioPreviewView::onFileChanged(void* context, const AudioPreviewFile& file)
{
    auto* self = static_cast<AudioPreviewView*>(context);
    if (self) {
        self->renderFile(file);
    }
}

void AudioPreviewView::onProgressChanged(void* context, const float& progressSec)
{
    auto* self = static_cast<AudioPreviewView*>(context);
    if (self) {
        self->renderProgress(progressSec);
    }
}

void AudioPreviewView::onSpeedChanged(void* context, const float& speed)
{
    auto* self = static_cast<AudioPreviewView*>(context);
    if (self) {
        self->renderSpeed(speed);
    }
}

}  // namespace files
