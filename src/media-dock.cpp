#include "media-dock.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QtCore/QByteArray>
#include <QtCore/QSignalBlocker>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtGui/QFont>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSlider>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr const char *DOCK_ID = "obs-box-layouts-media-monitor";

obs_data_t *action_settings = nullptr;
char *action_settings_path = nullptr;

void init_action_settings()
{
	char *config_directory = obs_module_config_path(nullptr);
	if (config_directory) {
		os_mkdirs(config_directory);
		bfree(config_directory);
	}
	action_settings_path = obs_module_config_path("media-actions.json");
	if (action_settings_path)
		action_settings = obs_data_create_from_json_file_safe(action_settings_path, "bak");
	if (!action_settings)
		action_settings = obs_data_create();
}

void save_action_settings()
{
	if (action_settings && action_settings_path &&
	    !obs_data_save_json_pretty_safe(action_settings, action_settings_path, "tmp", "bak"))
		blog(LOG_WARNING, "[obs-box-layouts] could not save media end actions");
}

bool action_is_enabled(const QString &media_uuid)
{
	if (!action_settings)
		return false;
	const QByteArray key = media_uuid.toUtf8();
	obs_data_t *action = obs_data_get_obj(action_settings, key.constData());
	if (!action)
		return false;
	const bool enabled = obs_data_get_bool(action, "enabled");
	obs_data_release(action);
	return enabled;
}

QString action_target_uuid(const QString &media_uuid)
{
	if (!action_settings)
		return {};
	const QByteArray key = media_uuid.toUtf8();
	obs_data_t *action = obs_data_get_obj(action_settings, key.constData());
	if (!action)
		return {};
	const QString target = QString::fromUtf8(obs_data_get_string(action, "target_scene_uuid"));
	obs_data_release(action);
	return target;
}

void set_action(const QString &media_uuid, bool enabled, const QString &target_uuid)
{
	if (!action_settings || media_uuid.isEmpty())
		return;
	obs_data_t *action = obs_data_create();
	obs_data_set_bool(action, "enabled", enabled);
	const QByteArray target = target_uuid.toUtf8();
	obs_data_set_string(action, "target_scene_uuid", target.constData());
	const QByteArray key = media_uuid.toUtf8();
	obs_data_set_obj(action_settings, key.constData(), action);
	obs_data_release(action);
	save_action_settings();
}

void shutdown_action_settings()
{
	if (action_settings) {
		save_action_settings();
		obs_data_release(action_settings);
		action_settings = nullptr;
	}
	bfree(action_settings_path);
	action_settings_path = nullptr;
}

QString tr_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString format_time(int64_t milliseconds)
{
	milliseconds = std::max<int64_t>(milliseconds, 0);
	const int64_t seconds = milliseconds / 1000;
	const int64_t hours = seconds / 3600;
	const int64_t minutes = (seconds % 3600) / 60;
	const int64_t remainder = seconds % 60;
	if (hours > 0)
		return QStringLiteral("%1:%2:%3")
			.arg(hours)
			.arg(minutes, 2, 10, QLatin1Char('0'))
			.arg(remainder, 2, 10, QLatin1Char('0'));
	return QStringLiteral("%1:%2").arg(minutes, 2, 10, QLatin1Char('0')).arg(remainder, 2, 10,
											 QLatin1Char('0'));
}

QString state_text(enum obs_media_state state)
{
	switch (state) {
	case OBS_MEDIA_STATE_PLAYING:
		return tr_text("Media.State.Playing");
	case OBS_MEDIA_STATE_OPENING:
		return tr_text("Media.State.Opening");
	case OBS_MEDIA_STATE_BUFFERING:
		return tr_text("Media.State.Buffering");
	case OBS_MEDIA_STATE_PAUSED:
		return tr_text("Media.State.Paused");
	case OBS_MEDIA_STATE_STOPPED:
		return tr_text("Media.State.Stopped");
	case OBS_MEDIA_STATE_ENDED:
		return tr_text("Media.State.Ended");
	case OBS_MEDIA_STATE_ERROR:
		return tr_text("Media.State.Error");
	case OBS_MEDIA_STATE_NONE:
	default:
		return tr_text("Media.State.Unknown");
	}
}

struct ScanContext {
	std::unordered_set<obs_source_t *> visited;
	std::vector<obs_source_t *> media;
	bool found_layout = false;
};

struct EnumContext {
	ScanContext *scan;
	bool inside_layout;
};

void scan_source(obs_source_t *source, bool inside_layout, ScanContext &scan);

void enum_child(obs_source_t *, obs_source_t *child, void *param)
{
	auto *context = static_cast<EnumContext *>(param);
	scan_source(child, context->inside_layout, *context->scan);
}

void scan_source(obs_source_t *source, bool inside_layout, ScanContext &scan)
{
	if (!source || !scan.visited.insert(source).second)
		return;

	const char *id = obs_source_get_unversioned_id(source);
	const bool is_layout = id && std::strcmp(id, "box_layout_source") == 0;
	inside_layout = inside_layout || is_layout;
	if (is_layout)
		scan.found_layout = true;

	const uint32_t flags = obs_source_get_output_flags(source);
	if (inside_layout && !is_layout && (flags & OBS_SOURCE_CONTROLLABLE_MEDIA)) {
		obs_source_t *reference = obs_source_get_ref(source);
		if (reference)
			scan.media.push_back(reference);
	}

	EnumContext context{&scan, inside_layout};
	obs_source_enum_active_sources(source, enum_child, &context);
}

class MediaRow final : public QWidget {
public:
	explicit MediaRow(obs_source_t *source, QWidget *parent = nullptr) : QWidget(parent)
	{
		setObjectName(QStringLiteral("mediaRow"));
		weak_source = obs_source_get_weak_source(source);
		media_uuid = QString::fromUtf8(obs_source_get_uuid(source));
		previous_state = obs_source_media_get_state(source);
		previous_time = std::max<int64_t>(obs_source_media_get_time(source), 0);
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(8, 8, 8, 8);
		root->setSpacing(5);

		auto *heading = new QHBoxLayout;
		name_label = new QLabel(QString::fromUtf8(obs_source_get_name(source)), this);
		QFont title_font = name_label->font();
		title_font.setBold(true);
		name_label->setFont(title_font);
		state_label = new QLabel(this);
		state_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		heading->addWidget(name_label, 1);
		heading->addWidget(state_label);
		root->addLayout(heading);

		time_label = new QLabel(this);
		root->addWidget(time_label);

		progress = new QSlider(Qt::Horizontal, this);
		progress->setRange(0, 1000);
		progress->setSingleStep(1);
		root->addWidget(progress);

		auto *controls = new QHBoxLayout;
		play_pause = new QPushButton(tr_text("Media.Pause"), this);
		restart = new QPushButton(tr_text("Media.Restart"), this);
		stop = new QPushButton(tr_text("Media.Stop"), this);
		controls->addWidget(play_pause);
		controls->addWidget(restart);
		controls->addWidget(stop);
		controls->addStretch(1);
		root->addLayout(controls);

		auto *end_action = new QHBoxLayout;
		auto_switch = new QCheckBox(tr_text("Media.AutoSwitch"), this);
		target_scene = new QComboBox(this);
		target_scene->setMinimumContentsLength(18);
		end_action->addWidget(auto_switch);
		end_action->addWidget(target_scene, 1);
		root->addLayout(end_action);
		refresh_scene_choices();
		auto_switch->setChecked(action_is_enabled(media_uuid));
		target_scene->setEnabled(auto_switch->isChecked());

		connect(play_pause, &QPushButton::clicked, this, [this]() {
			with_source([](obs_source_t *media) {
				const enum obs_media_state state = obs_source_media_get_state(media);
				if (state == OBS_MEDIA_STATE_STOPPED || state == OBS_MEDIA_STATE_ENDED ||
				    state == OBS_MEDIA_STATE_ERROR)
					obs_source_media_restart(media);
				else
					obs_source_media_play_pause(media, state == OBS_MEDIA_STATE_PLAYING);
			});
		});
		connect(restart, &QPushButton::clicked, this,
			[this]() { with_source([](obs_source_t *media) { obs_source_media_restart(media); }); });
		connect(stop, &QPushButton::clicked, this,
			[this]() { with_source([](obs_source_t *media) { obs_source_media_stop(media); }); });
		connect(progress, &QSlider::sliderReleased, this, [this]() {
			suppress_loop_detection_once = true;
			with_source([this](obs_source_t *media) {
				const int64_t duration = obs_source_media_get_duration(media);
				if (duration > 0)
					obs_source_media_set_time(media, duration * progress->value() / 1000);
			});
		});
		connect(auto_switch, &QCheckBox::toggled, this, [this](bool checked) {
			target_scene->setEnabled(checked);
			set_action(media_uuid, checked, target_scene->currentData().toString());
		});
		connect(target_scene, &QComboBox::currentIndexChanged, this, [this](int) {
			set_action(media_uuid, auto_switch->isChecked(), target_scene->currentData().toString());
		});
	}

	~MediaRow() override
	{
		if (weak_source)
			obs_weak_source_release(weak_source);
	}

	bool represents(obs_source_t *source) const
	{
		obs_source_t *current = obs_weak_source_get_source(weak_source);
		const bool same = current == source;
		if (current)
			obs_source_release(current);
		return same;
	}

	QString refresh()
	{
		refresh_scene_choices();
		obs_source_t *source = obs_weak_source_get_source(weak_source);
		if (!source)
			return {};

		const enum obs_media_state state = obs_source_media_get_state(source);
		const int64_t duration = std::max<int64_t>(obs_source_media_get_duration(source), 0);
		const int64_t current = std::clamp<int64_t>(obs_source_media_get_time(source), 0, duration > 0 ? duration : INT64_MAX);
		const int64_t remaining = duration > 0 ? std::max<int64_t>(duration - current, 0) : 0;

		name_label->setText(QString::fromUtf8(obs_source_get_name(source)));
		state_label->setText(state_text(state));
		if (duration > 0) {
			time_label->setText(tr_text("Media.TimeLine")
						    .arg(format_time(current), format_time(duration), format_time(remaining)));
			if (!progress->isSliderDown())
				progress->setValue(static_cast<int>(current * 1000 / duration));
			progress->setEnabled(true);
		} else {
			time_label->setText(tr_text("Media.DurationUnknown"));
			progress->setValue(0);
			progress->setEnabled(false);
		}

		play_pause->setText(state == OBS_MEDIA_STATE_PLAYING ? tr_text("Media.Pause") : tr_text("Media.Play"));
		set_alert_style(state, remaining, duration);
		const int64_t loop_window = duration > 0 ? std::min<int64_t>(2000, std::max<int64_t>(duration / 4, 250))
								      : 0;
		const bool loop_wrapped = !suppress_loop_detection_once && state == OBS_MEDIA_STATE_PLAYING &&
			previous_state == OBS_MEDIA_STATE_PLAYING && duration > 0 &&
			previous_time >= duration - loop_window && current <= loop_window;
		const bool ended_state = state == OBS_MEDIA_STATE_ENDED &&
			(previous_state == OBS_MEDIA_STATE_PLAYING || previous_state == OBS_MEDIA_STATE_BUFFERING);
		const bool ended_now = ended_state || loop_wrapped;
		if (state != previous_state)
			blog(LOG_INFO, "[obs-box-layouts] media '%s' state %d -> %d at %lld/%lld ms",
			     obs_source_get_name(source), (int)previous_state, (int)state, (long long)current,
			     (long long)duration);
		if (loop_wrapped)
			blog(LOG_INFO, "[obs-box-layouts] media '%s' loop completion detected", obs_source_get_name(source));
		suppress_loop_detection_once = false;
		previous_state = state;
		previous_time = current;
		const QString target = ended_now && auto_switch->isChecked() ? target_scene->currentData().toString()
										      : QString();
		obs_source_release(source);
		return target;
	}

private:
	void refresh_scene_choices()
	{
		struct obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		QString signature;
		for (size_t i = 0; i < scenes.sources.num; i++) {
			obs_source_t *scene = scenes.sources.array[i];
			signature += QString::fromUtf8(obs_source_get_uuid(scene));
			signature += QLatin1Char('|');
			signature += QString::fromUtf8(obs_source_get_name(scene));
			signature += QLatin1Char('\n');
		}
		if (signature == scene_signature) {
			obs_frontend_source_list_free(&scenes);
			return;
		}

		const QString saved_target = action_target_uuid(media_uuid);
		const QSignalBlocker blocker(target_scene);
		target_scene->clear();
		target_scene->addItem(tr_text("Media.ChooseScene"), QString());
		for (size_t i = 0; i < scenes.sources.num; i++) {
			obs_source_t *scene = scenes.sources.array[i];
			target_scene->addItem(QString::fromUtf8(obs_source_get_name(scene)),
					      QString::fromUtf8(obs_source_get_uuid(scene)));
		}
		const int saved_index = target_scene->findData(QVariant(saved_target));
		target_scene->setCurrentIndex(saved_index >= 0 ? saved_index : 0);
		scene_signature = signature;
		obs_frontend_source_list_free(&scenes);
	}

	template<typename Callback> void with_source(Callback callback)
	{
		obs_source_t *source = obs_weak_source_get_source(weak_source);
		if (!source)
			return;
		callback(source);
		obs_source_release(source);
	}

	void set_alert_style(enum obs_media_state state, int64_t remaining, int64_t duration)
	{
		QString color;
		if (state == OBS_MEDIA_STATE_PLAYING && duration > 0) {
			if (remaining <= 5000)
				color = QStringLiteral("rgba(190, 35, 45, 115)");
			else if (remaining <= 10000)
				color = QStringLiteral("rgba(220, 105, 20, 105)");
			else if (remaining <= 30000)
				color = QStringLiteral("rgba(215, 170, 25, 85)");
		}
		setStyleSheet(color.isEmpty()
				      ? QString()
				      : QStringLiteral("QWidget#mediaRow { background: %1; border-radius: 6px; }")
						.arg(color));
	}

	obs_weak_source_t *weak_source = nullptr;
	QString media_uuid;
	QString scene_signature;
	enum obs_media_state previous_state = OBS_MEDIA_STATE_NONE;
	int64_t previous_time = 0;
	bool suppress_loop_detection_once = false;
	QLabel *name_label = nullptr;
	QLabel *state_label = nullptr;
	QLabel *time_label = nullptr;
	QSlider *progress = nullptr;
	QPushButton *play_pause = nullptr;
	QPushButton *restart = nullptr;
	QPushButton *stop = nullptr;
	QCheckBox *auto_switch = nullptr;
	QComboBox *target_scene = nullptr;
};

class MediaMonitor final : public QWidget {
public:
	MediaMonitor()
	{
		setMinimumWidth(330);
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(8, 8, 8, 8);
		root->setSpacing(7);

		program_label = new QLabel(this);
		QFont program_font = program_label->font();
		program_font.setBold(true);
		program_label->setFont(program_font);
		root->addWidget(program_label);

		legend_label = new QLabel(tr_text("Media.AlertLegend"), this);
		legend_label->setWordWrap(true);
		root->addWidget(legend_label);

		empty_label = new QLabel(tr_text("Media.NoLayoutProgram"), this);
		empty_label->setWordWrap(true);
		empty_label->setAlignment(Qt::AlignCenter);
		root->addWidget(empty_label, 1);

		scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		rows_container = new QWidget(scroll);
		rows_layout = new QVBoxLayout(rows_container);
		rows_layout->setContentsMargins(0, 0, 0, 0);
		rows_layout->setSpacing(7);
		rows_layout->addStretch(1);
		scroll->setWidget(rows_container);
		root->addWidget(scroll, 1);

		timer = new QTimer(this);
		timer->setInterval(250);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
		timer->start();
		refresh();
	}

private:
	void refresh()
	{
		ScanContext scan;
		obs_source_t *program = obs_frontend_get_current_scene();
		QString scene_name;
		if (program) {
			scene_name = QString::fromUtf8(obs_source_get_name(program));
			scan_source(program, false, scan);
			obs_source_release(program);
		}

		if (scan.found_layout)
			program_label->setText(tr_text("Media.ProgramScene").arg(scene_name));
		else
			program_label->setText(tr_text("Media.ProgramInactive"));

		const bool same_sources = rows.size() == scan.media.size() &&
			std::equal(rows.begin(), rows.end(), scan.media.begin(),
				   [](MediaRow *row, obs_source_t *source) { return row->represents(source); });
		if (!same_sources)
			rebuild_rows(scan.media);

		QString switch_target;
		for (MediaRow *row : rows) {
			const QString candidate = row->refresh();
			if (switch_target.isEmpty() && !candidate.isEmpty())
				switch_target = candidate;
		}

		const bool has_media = !rows.empty();
		scroll->setVisible(has_media);
		empty_label->setVisible(!has_media);
		legend_label->setVisible(has_media);
		if (!has_media)
			empty_label->setText(scan.found_layout ? tr_text("Media.NoMediaProgram")
								       : tr_text("Media.NoLayoutProgram"));

		for (obs_source_t *source : scan.media)
			obs_source_release(source);

		if (!switch_target.isEmpty())
			switch_to_scene(switch_target);
	}

	void switch_to_scene(const QString &target_uuid)
	{
		const QByteArray uuid = target_uuid.toUtf8();
		obs_source_t *target = obs_get_source_by_uuid(uuid.constData());
		if (!target || obs_source_get_type(target) != OBS_SOURCE_TYPE_SCENE) {
			if (target)
				obs_source_release(target);
			return;
		}

		obs_source_t *current = obs_frontend_get_current_scene();
		const bool already_current = current == target;
		if (current)
			obs_source_release(current);
		if (!already_current) {
			blog(LOG_INFO, "[obs-box-layouts] media ended; switching Program to scene '%s'",
			     obs_source_get_name(target));
			obs_frontend_set_current_scene(target);
		}
		obs_source_release(target);
	}

	void rebuild_rows(const std::vector<obs_source_t *> &sources)
	{
		for (MediaRow *row : rows) {
			rows_layout->removeWidget(row);
			delete row;
		}
		rows.clear();
		for (obs_source_t *source : sources) {
			auto *row = new MediaRow(source, rows_container);
			rows_layout->insertWidget(rows_layout->count() - 1, row);
			rows.push_back(row);
		}
	}

	QLabel *program_label = nullptr;
	QLabel *legend_label = nullptr;
	QLabel *empty_label = nullptr;
	QScrollArea *scroll = nullptr;
	QWidget *rows_container = nullptr;
	QVBoxLayout *rows_layout = nullptr;
	QTimer *timer = nullptr;
	std::vector<MediaRow *> rows;
};

MediaMonitor *monitor = nullptr;

} // namespace

extern "C" bool media_dock_init(void)
{
	if (monitor)
		return true;
	init_action_settings();
	monitor = new MediaMonitor;
	if (!obs_frontend_add_dock_by_id(DOCK_ID, obs_module_text("Media.DockTitle"), monitor)) {
		delete monitor;
		monitor = nullptr;
		shutdown_action_settings();
		return false;
	}
	QTimer::singleShot(0, monitor, []() {
		if (monitor && monitor->parentWidget())
			monitor->parentWidget()->show();
	});
	blog(LOG_INFO, "[obs-box-layouts] media monitor dock registered");
	return true;
}

extern "C" void media_dock_shutdown(void)
{
	if (!monitor)
		return;
	obs_frontend_remove_dock(DOCK_ID);
	monitor = nullptr;
	shutdown_action_settings();
}
