#include "playlist-dock.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QMetaObject>
#include <QtCore/QSignalBlocker>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr const char *DOCK_ID = "obs-box-layouts-playlist";
constexpr int MAX_BOXES = 6;
constexpr int PRESET_CUSTOM = 8;
constexpr int DEFAULT_STILL_DURATION_MS = 5000;
constexpr int HOTKEY_DIRECT_ITEMS = 6;
constexpr int HOTKEY_ACTION_PREVIOUS = -1;
constexpr int HOTKEY_ACTION_NEXT = -2;
constexpr int TARGET_PREVIEW = 0;
constexpr int TARGET_PROGRAM = 1;
constexpr int FIT_FILL = 0;
constexpr int FIT_CONTAIN = 1;
constexpr int FIT_STRETCH = 2;
constexpr int FIT_MANUAL = 3;

struct PlaylistItem {
	QString source_uuid;
	QString source_name;
	int duration_ms = DEFAULT_STILL_DURATION_MS;
	int fit_mode = FIT_FILL;
	double zoom = 1.0;
	double pan_x = 0.0;
	double pan_y = 0.0;
};

struct PlaylistState {
	std::vector<PlaylistItem> items;
	int current = -1;
	bool auto_advance = false;
	bool loop = false;
};

struct SourceChoice {
	QString uuid;
	QString name;
	bool scene = false;
};

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
	return QStringLiteral("%1:%2").arg(minutes, 2, 10, QLatin1Char('0')).arg(remainder, 2, 10, QLatin1Char('0'));
}

QString fit_mode_name(int fit_mode)
{
	switch (fit_mode) {
	case FIT_CONTAIN:
		return tr_text("Fit.Contain");
	case FIT_STRETCH:
		return tr_text("Fit.Stretch");
	case FIT_MANUAL:
		return tr_text("Fit.Manual");
	case FIT_FILL:
	default:
		return tr_text("Fit.Fill");
	}
}

int preset_box_count(int preset)
{
	switch (preset) {
	case 0:
		return 1;
	case 1:
	case 2:
		return 2;
	case 3:
	case 4:
		return 3;
	case 5:
	case 6:
		return 4;
	case 7:
	default:
		return 6;
	}
}

int layout_box_count(obs_source_t *layout)
{
	if (!layout)
		return 0;
	obs_data_t *settings = obs_source_get_settings(layout);
	const int preset = static_cast<int>(obs_data_get_int(settings, "preset"));
	const int count = preset == PRESET_CUSTOM
				  ? std::clamp<int>(static_cast<int>(obs_data_get_int(settings, "custom_box_count")), 1,
						    MAX_BOXES)
				  : preset_box_count(preset);
	obs_data_release(settings);
	return count;
}

void playlist_key(char *buffer, size_t size, int box, const char *suffix)
{
	snprintf(buffer, size, "playlist_box_%d_%s", box + 1, suffix);
}

PlaylistState load_playlist(obs_source_t *layout, int box)
{
	PlaylistState state;
	if (!layout || box < 0 || box >= MAX_BOXES)
		return state;

	obs_data_t *settings = obs_source_get_settings(layout);
	char key[64];
	playlist_key(key, sizeof(key), box, "items");
	obs_data_array_t *items = obs_data_get_array(settings, key);
	if (items) {
		const size_t count = obs_data_array_count(items);
		state.items.reserve(count);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(items, i);
			PlaylistItem entry;
			entry.source_uuid = QString::fromUtf8(obs_data_get_string(item, "source_uuid"));
			entry.source_name = QString::fromUtf8(obs_data_get_string(item, "source_name"));
			entry.duration_ms =
				std::clamp<int>(static_cast<int>(obs_data_get_int(item, "duration_ms")), 500, 3600000);
			entry.fit_mode = std::clamp<int>(static_cast<int>(obs_data_get_int(item, "fit_mode")), FIT_FILL,
							 FIT_MANUAL);
			entry.zoom = obs_data_has_user_value(item, "zoom") ? obs_data_get_double(item, "zoom") : 1.0;
			entry.zoom = std::clamp(entry.zoom, 0.1, 4.0);
			entry.pan_x = std::clamp(obs_data_get_double(item, "pan_x"), -100.0, 100.0);
			entry.pan_y = std::clamp(obs_data_get_double(item, "pan_y"), -100.0, 100.0);
			state.items.push_back(entry);
			obs_data_release(item);
		}
		obs_data_array_release(items);
	}
	playlist_key(key, sizeof(key), box, "current");
	state.current = obs_data_has_user_value(settings, key) ? static_cast<int>(obs_data_get_int(settings, key)) : -1;
	if (state.current < -1 || state.current >= static_cast<int>(state.items.size()))
		state.current = -1;
	playlist_key(key, sizeof(key), box, "auto");
	state.auto_advance = obs_data_get_bool(settings, key);
	playlist_key(key, sizeof(key), box, "loop");
	state.loop = obs_data_get_bool(settings, key);
	obs_data_release(settings);
	return state;
}

QString assigned_box_source(obs_source_t *layout, int box)
{
	if (!layout || box < 0 || box >= MAX_BOXES)
		return {};
	obs_data_t *settings = obs_source_get_settings(layout);
	char key[64];
	snprintf(key, sizeof(key), "box_%d_source", box + 1);
	const QString name = QString::fromUtf8(obs_data_get_string(settings, key));
	obs_data_release(settings);
	return name;
}

void save_playlist(obs_source_t *layout, int box, const PlaylistState &state, const PlaylistItem *take_item = nullptr)
{
	if (!layout || box < 0 || box >= MAX_BOXES)
		return;
	obs_data_t *settings = obs_source_get_settings(layout);
	char key[64];
	playlist_key(key, sizeof(key), box, "items");
	obs_data_array_t *items = obs_data_array_create();
	for (const PlaylistItem &entry : state.items) {
		obs_data_t *item = obs_data_create();
		const QByteArray uuid = entry.source_uuid.toUtf8();
		const QByteArray name = entry.source_name.toUtf8();
		obs_data_set_string(item, "source_uuid", uuid.constData());
		obs_data_set_string(item, "source_name", name.constData());
		obs_data_set_int(item, "duration_ms", entry.duration_ms);
		obs_data_set_int(item, "fit_mode", entry.fit_mode);
		obs_data_set_double(item, "zoom", entry.zoom);
		obs_data_set_double(item, "pan_x", entry.pan_x);
		obs_data_set_double(item, "pan_y", entry.pan_y);
		obs_data_array_push_back(items, item);
		obs_data_release(item);
	}
	obs_data_set_array(settings, key, items);
	obs_data_array_release(items);
	playlist_key(key, sizeof(key), box, "current");
	obs_data_set_int(settings, key, state.current);
	playlist_key(key, sizeof(key), box, "auto");
	obs_data_set_bool(settings, key, state.auto_advance);
	playlist_key(key, sizeof(key), box, "loop");
	obs_data_set_bool(settings, key, state.loop);
	if (take_item) {
		snprintf(key, sizeof(key), "box_%d_source", box + 1);
		const QByteArray name = take_item->source_name.toUtf8();
		obs_data_set_string(settings, key, name.constData());
		snprintf(key, sizeof(key), "box_%d_fit_mode", box + 1);
		obs_data_set_int(settings, key, take_item->fit_mode);
		snprintf(key, sizeof(key), "box_%d_zoom", box + 1);
		obs_data_set_double(settings, key, take_item->zoom);
		snprintf(key, sizeof(key), "box_%d_pan_x", box + 1);
		obs_data_set_double(settings, key, take_item->pan_x);
		snprintf(key, sizeof(key), "box_%d_pan_y", box + 1);
		obs_data_set_double(settings, key, take_item->pan_y);
	}
	obs_source_update(layout, settings);
	obs_data_release(settings);
}

PlaylistItem current_box_framing(obs_source_t *layout, int box)
{
	PlaylistItem framing;
	if (!layout || box < 0 || box >= MAX_BOXES)
		return framing;
	obs_data_t *settings = obs_source_get_settings(layout);
	char key[64];
	snprintf(key, sizeof(key), "box_%d_fit_mode", box + 1);
	framing.fit_mode = std::clamp<int>(static_cast<int>(obs_data_get_int(settings, key)), FIT_FILL, FIT_MANUAL);
	snprintf(key, sizeof(key), "box_%d_zoom", box + 1);
	framing.zoom = std::clamp(obs_data_get_double(settings, key), 0.1, 4.0);
	snprintf(key, sizeof(key), "box_%d_pan_x", box + 1);
	framing.pan_x = std::clamp(obs_data_get_double(settings, key), -100.0, 100.0);
	snprintf(key, sizeof(key), "box_%d_pan_y", box + 1);
	framing.pan_y = std::clamp(obs_data_get_double(settings, key), -100.0, 100.0);
	obs_data_release(settings);
	return framing;
}

obs_source_t *resolve_item(const PlaylistItem &item)
{
	obs_source_t *source = nullptr;
	if (!item.source_uuid.isEmpty()) {
		const QByteArray uuid = item.source_uuid.toUtf8();
		source = obs_get_source_by_uuid(uuid.constData());
	}
	if (!source && !item.source_name.isEmpty()) {
		const QByteArray name = item.source_name.toUtf8();
		source = obs_get_source_by_name(name.constData());
	}
	return source;
}

struct LayoutScan {
	std::unordered_set<obs_source_t *> visited;
	std::vector<obs_source_t *> layouts;
};

void scan_layouts(obs_source_t *source, LayoutScan &scan);

void enum_layout_child(obs_source_t *, obs_source_t *child, void *param)
{
	scan_layouts(child, *static_cast<LayoutScan *>(param));
}

void scan_layouts(obs_source_t *source, LayoutScan &scan)
{
	if (!source || !scan.visited.insert(source).second)
		return;
	const char *id = obs_source_get_unversioned_id(source);
	if (id && std::strcmp(id, "box_layout_source") == 0) {
		obs_source_t *reference = obs_source_get_ref(source);
		if (reference)
			scan.layouts.push_back(reference);
	}
	obs_source_enum_active_sources(source, enum_layout_child, &scan);
}

struct ChoiceScan {
	obs_source_t *layout = nullptr;
	std::vector<SourceChoice> choices;
};

bool add_source_choice(void *param, obs_source_t *source)
{
	auto *scan = static_cast<ChoiceScan *>(param);
	if (source == scan->layout || obs_source_removed(source) ||
	    !(obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO))
		return true;
	scan->choices.push_back({QString::fromUtf8(obs_source_get_uuid(source)),
				 QString::fromUtf8(obs_source_get_name(source)),
				 obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE});
	return true;
}

class PlaylistDock final : public QWidget {
public:
	PlaylistDock()
	{
		setMinimumWidth(390);
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(8, 8, 8, 8);
		root->setSpacing(7);

		program_label = new QLabel(tr_text("Playlist.NoLayout"), this);
		QFont heading = program_label->font();
		heading.setBold(true);
		program_label->setFont(heading);
		root->addWidget(program_label);

		target_selector = new QComboBox(this);
		root->addWidget(target_selector);

		layout_selector = new QComboBox(this);
		root->addWidget(layout_selector);
		box_selector = new QComboBox(this);
		root->addWidget(box_selector);

		warning = new QLabel(tr_text("Playlist.LiveWarning"), this);
		warning->setAlignment(Qt::AlignCenter);
		warning->setStyleSheet(QStringLiteral("background:#9f2530;color:white;padding:5px;font-weight:bold;"));
		root->addWidget(warning);

		auto *add_row = new QHBoxLayout;
		source_picker = new QComboBox(this);
		source_picker->setMinimumContentsLength(18);
		add_button = new QPushButton(tr_text("Playlist.Add"), this);
		add_row->addWidget(source_picker, 1);
		add_row->addWidget(add_button);
		root->addLayout(add_row);

		queue = new QListWidget(this);
		queue->setMinimumHeight(150);
		queue->setAlternatingRowColors(true);
		root->addWidget(queue, 1);

		auto *edit_row = new QHBoxLayout;
		remove_button = new QPushButton(tr_text("Playlist.Remove"), this);
		save_framing_button = new QPushButton(tr_text("Playlist.SaveFraming"), this);
		up_button = new QPushButton(QStringLiteral("↑"), this);
		down_button = new QPushButton(QStringLiteral("↓"), this);
		edit_row->addWidget(remove_button);
		edit_row->addWidget(save_framing_button);
		edit_row->addStretch(1);
		edit_row->addWidget(up_button);
		edit_row->addWidget(down_button);
		root->addLayout(edit_row);

		auto *duration_row = new QHBoxLayout;
		duration_row->addWidget(new QLabel(tr_text("Playlist.StillDuration"), this));
		duration = new QSpinBox(this);
		duration->setRange(1, 3600);
		duration->setValue(DEFAULT_STILL_DURATION_MS / 1000);
		duration->setSuffix(QStringLiteral(" s"));
		duration_row->addWidget(duration);
		duration_row->addStretch(1);
		root->addLayout(duration_row);

		auto *take_row = new QHBoxLayout;
		previous_button = new QPushButton(tr_text("Playlist.Previous"), this);
		take_button = new QPushButton(tr_text("Playlist.Take"), this);
		next_button = new QPushButton(tr_text("Playlist.Next"), this);
		take_button->setStyleSheet(QStringLiteral("font-weight:bold;padding:6px;"));
		take_row->addWidget(previous_button);
		take_row->addWidget(take_button, 1);
		take_row->addWidget(next_button);
		root->addLayout(take_row);

		auto_advance = new QCheckBox(tr_text("Playlist.AutoAdvance"), this);
		loop = new QCheckBox(tr_text("Playlist.Loop"), this);
		root->addWidget(auto_advance);
		root->addWidget(loop);
		auto *hotkey_help = new QLabel(tr_text("Hotkey.Help"), this);
		hotkey_help->setWordWrap(true);
		hotkey_help->setStyleSheet(QStringLiteral("color:#aeb4bd;font-size:11px;"));
		root->addWidget(hotkey_help);

		status = new QLabel(tr_text("Playlist.NoCurrent"), this);
		status->setWordWrap(true);
		root->addWidget(status);
		progress = new QProgressBar(this);
		progress->setRange(0, 1000);
		progress->setTextVisible(false);
		root->addWidget(progress);

		connect(layout_selector, &QComboBox::currentIndexChanged, this, [this](int) {
			if (!preview_target())
				preferred_program_layout_uuid = layout_selector->currentData().toString();
			reset_runtime();
			playlist_signature.clear();
			refresh_selected_layout();
		});
		connect(target_selector, &QComboBox::currentIndexChanged, this, [this](int) {
			program_uuid.clear();
			layout_signature.clear();
			source_signature.clear();
			playlist_signature.clear();
			reset_runtime();
			refresh();
		});
		connect(box_selector, &QComboBox::currentIndexChanged, this, [this](int) {
			reset_runtime();
			playlist_signature.clear();
			refresh_playlist();
		});
		connect(queue, &QListWidget::currentRowChanged, this, [this](int) { update_selection_controls(); });
		connect(queue, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { take_selected(); });
		connect(add_button, &QPushButton::clicked, this, [this]() { add_item(); });
		connect(source_picker, &QComboBox::currentIndexChanged, this, [this](int) {
			add_button->setEnabled(box_selector->count() > 0 &&
					       !source_picker->currentData().toString().isEmpty());
		});
		connect(remove_button, &QPushButton::clicked, this, [this]() { remove_item(); });
		connect(save_framing_button, &QPushButton::clicked, this, [this]() { save_selected_framing(); });
		connect(up_button, &QPushButton::clicked, this, [this]() { move_item(-1); });
		connect(down_button, &QPushButton::clicked, this, [this]() { move_item(1); });
		connect(take_button, &QPushButton::clicked, this, [this]() { take_selected(); });
		connect(previous_button, &QPushButton::clicked, this, [this]() { navigate(-1); });
		connect(next_button, &QPushButton::clicked, this, [this]() { navigate(1); });
		connect(duration, &QSpinBox::valueChanged, this, [this](int seconds) {
			if (!updating_ui)
				set_selected_duration(seconds * 1000);
		});
		connect(auto_advance, &QCheckBox::toggled, this, [this](bool enabled) {
			if (!updating_ui)
				set_option(true, enabled);
		});
		connect(loop, &QCheckBox::toggled, this, [this](bool enabled) {
			if (!updating_ui)
				set_option(false, enabled);
		});

		timer = new QTimer(this);
		timer->setInterval(250);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
		timer->start();
		refresh();
	}

	void trigger_hotkey(int box, int action)
	{
		if (box < 0 || box >= MAX_BOXES)
			return;
		obs_source_t *layout = hotkey_layout();
		if (!layout || box >= layout_box_count(layout)) {
			if (layout)
				obs_source_release(layout);
			return;
		}
		const PlaylistState state = load_playlist(layout, box);
		if (state.items.empty()) {
			obs_source_release(layout);
			return;
		}
		int target = action;
		if (action == HOTKEY_ACTION_NEXT) {
			target = state.current < 0 ? 0 : (state.current + 1) % static_cast<int>(state.items.size());
		} else if (action == HOTKEY_ACTION_PREVIOUS) {
			target = state.current < 0 ? static_cast<int>(state.items.size()) - 1
						   : (state.current - 1 + static_cast<int>(state.items.size())) %
							     static_cast<int>(state.items.size());
		}
		if (target >= 0 && target < static_cast<int>(state.items.size()))
			take_index_on_layout(layout, box, target, false);
		obs_source_release(layout);
	}

private:
	bool preview_target() const
	{
		return obs_frontend_preview_program_mode_active() &&
		       target_selector->currentData().toInt() == TARGET_PREVIEW;
	}

	void refresh_target_choices()
	{
		const bool studio = obs_frontend_preview_program_mode_active();
		if (studio == studio_mode_known && target_selector->count() > 0)
			return;
		const int previous = target_selector->count() > 0 ? target_selector->currentData().toInt()
								  : TARGET_PREVIEW;
		const QSignalBlocker blocker(target_selector);
		target_selector->clear();
		if (studio)
			target_selector->addItem(tr_text("Target.Preview"), TARGET_PREVIEW);
		target_selector->addItem(tr_text("Target.Program"), TARGET_PROGRAM);
		const int retained = target_selector->findData(studio ? previous : TARGET_PROGRAM);
		target_selector->setCurrentIndex(retained >= 0 ? retained : 0);
		studio_mode_known = studio;
		program_uuid.clear();
		layout_signature.clear();
	}

	obs_source_t *selected_layout() const
	{
		const QByteArray uuid = layout_selector->currentData().toString().toUtf8();
		return uuid.isEmpty() ? nullptr : obs_get_source_by_uuid(uuid.constData());
	}

	int selected_box() const { return box_selector->currentData().toInt(); }

	obs_source_t *hotkey_layout() const
	{
		LayoutScan scan;
		obs_source_t *program = obs_frontend_get_current_scene();
		if (program) {
			scan_layouts(program, scan);
			obs_source_release(program);
		}
		if (scan.layouts.empty())
			return nullptr;
		const QString preferred_uuid = preferred_program_layout_uuid;
		size_t selected_index = 0;
		if (!preferred_uuid.isEmpty()) {
			for (size_t i = 0; i < scan.layouts.size(); i++) {
				if (preferred_uuid == QString::fromUtf8(obs_source_get_uuid(scan.layouts[i]))) {
					selected_index = i;
					break;
				}
			}
		}
		obs_source_t *selected = scan.layouts[selected_index];
		for (size_t i = 0; i < scan.layouts.size(); i++)
			if (i != selected_index)
				obs_source_release(scan.layouts[i]);
		return selected;
	}

	void refresh()
	{
		refresh_target_choices();
		LayoutScan scan;
		const bool preview = preview_target();
		obs_source_t *program = preview ? obs_frontend_get_current_preview_scene()
						: obs_frontend_get_current_scene();
		QString next_program_uuid;
		QString program_name;
		if (program) {
			next_program_uuid = QString::fromUtf8(obs_source_get_uuid(program));
			program_name = QString::fromUtf8(obs_source_get_name(program));
			scan_layouts(program, scan);
			obs_source_release(program);
		}
		if (next_program_uuid != program_uuid) {
			program_uuid = next_program_uuid;
			reset_runtime();
		}

		QString signature;
		for (obs_source_t *layout : scan.layouts)
			signature += QString::fromUtf8(obs_source_get_uuid(layout)) + QLatin1Char('|') +
				     QString::fromUtf8(obs_source_get_name(layout)) + QLatin1Char('\n');
		if (signature != layout_signature) {
			const QString selected_uuid = layout_selector->currentData().toString();
			const QSignalBlocker blocker(layout_selector);
			layout_selector->clear();
			for (obs_source_t *layout : scan.layouts)
				layout_selector->addItem(QString::fromUtf8(obs_source_get_name(layout)),
							 QString::fromUtf8(obs_source_get_uuid(layout)));
			const int retained = layout_selector->findData(selected_uuid);
			if (retained >= 0)
				layout_selector->setCurrentIndex(retained);
			if (!preview)
				preferred_program_layout_uuid = layout_selector->currentData().toString();
			layout_signature = signature;
			refresh_selected_layout();
		}
		preview_shared = false;
		if (preview) {
			LayoutScan program_scan;
			obs_source_t *on_air = obs_frontend_get_current_scene();
			if (on_air) {
				scan_layouts(on_air, program_scan);
				obs_source_release(on_air);
			}
			const QString selected_uuid = layout_selector->currentData().toString();
			for (obs_source_t *candidate : program_scan.layouts) {
				if (selected_uuid == QString::fromUtf8(obs_source_get_uuid(candidate)))
					preview_shared = true;
				obs_source_release(candidate);
			}
		}

		program_label->setText(scan.layouts.empty()
					       ? tr_text(preview ? "Playlist.NoPreviewLayout" : "Playlist.NoLayout")
					       : tr_text(preview ? "Playlist.PreviewScene" : "Playlist.ProgramScene")
							 .arg(program_name));
		warning->setText(tr_text(preview_shared
						 ? "Playlist.SharedWarning"
						 : (preview ? "Playlist.PreviewWarning" : "Playlist.LiveWarning")));
		warning->setStyleSheet(
			preview_shared ? QStringLiteral("background:#a86616;color:white;padding:5px;font-weight:bold;")
			: preview      ? QStringLiteral("background:#176a91;color:white;padding:5px;font-weight:bold;")
				  : QStringLiteral("background:#9f2530;color:white;padding:5px;font-weight:bold;"));
		take_button->setText(tr_text(preview ? "Playlist.LoadPreview" : "Playlist.Take"));
		layout_selector->setVisible(scan.layouts.size() > 1);
		for (obs_source_t *layout : scan.layouts)
			obs_source_release(layout);

		refresh_source_choices();
		refresh_playlist();
		tick_current();
	}

	void refresh_selected_layout()
	{
		obs_source_t *layout = selected_layout();
		const int count = layout_box_count(layout);
		if (layout)
			obs_source_release(layout);
		const int previous_box = selected_box();
		const QSignalBlocker blocker(box_selector);
		box_selector->clear();
		for (int i = 0; i < count; i++)
			box_selector->addItem(tr_text("Playlist.Box").arg(i + 1), i);
		const int retained = box_selector->findData(previous_box);
		if (retained >= 0)
			box_selector->setCurrentIndex(retained);
		playlist_signature.clear();
		refresh_source_choices();
		refresh_playlist();
	}

	void refresh_source_choices()
	{
		obs_source_t *layout = selected_layout();
		ChoiceScan scan;
		scan.layout = layout;
		obs_enum_scenes(add_source_choice, &scan);
		obs_enum_sources(add_source_choice, &scan);
		std::sort(scan.choices.begin(), scan.choices.end(), [](const SourceChoice &a, const SourceChoice &b) {
			if (a.scene != b.scene)
				return a.scene;
			return QString::localeAwareCompare(a.name, b.name) < 0;
		});
		QString signature;
		for (const SourceChoice &choice : scan.choices)
			signature += choice.uuid + QLatin1Char('|') + choice.name + QLatin1Char('|') +
				     QString::number(choice.scene ? 1 : 0) + QLatin1Char('\n');
		if (signature != source_signature) {
			const QString selected_uuid = source_picker->currentData().toString();
			const QSignalBlocker blocker(source_picker);
			source_picker->clear();
			source_picker->addItem(tr_text("Playlist.ChooseSource"), QString());
			for (const SourceChoice &choice : scan.choices)
				source_picker->addItem(tr_text(choice.scene ? "Playlist.SceneChoice"
									    : "Playlist.SourceChoice")
							       .arg(choice.name),
						       choice.uuid);
			const int retained = source_picker->findData(selected_uuid);
			source_picker->setCurrentIndex(retained >= 0 ? retained : 0);
			source_signature = signature;
		}
		if (layout)
			obs_source_release(layout);
	}

	QString state_signature(const PlaylistState &state) const
	{
		QString signature = QStringLiteral("%1|%2|%3\n")
					    .arg(state.current)
					    .arg(state.auto_advance ? 1 : 0)
					    .arg(state.loop ? 1 : 0);
		for (const PlaylistItem &item : state.items)
			signature += item.source_uuid + QLatin1Char('|') + item.source_name + QLatin1Char('|') +
				     QString::number(item.duration_ms) + QLatin1Char('|') +
				     QString::number(item.fit_mode) + QLatin1Char('|') + QString::number(item.zoom) +
				     QLatin1Char('|') + QString::number(item.pan_x) + QLatin1Char('|') +
				     QString::number(item.pan_y) + QLatin1Char('\n');
		return signature;
	}

	void refresh_playlist()
	{
		obs_source_t *layout = selected_layout();
		const bool available = layout && box_selector->count() > 0;
		if (!available) {
			if (layout)
				obs_source_release(layout);
			queue->clear();
			playlist_signature.clear();
			set_available(false);
			return;
		}
		const PlaylistState state = load_playlist(layout, selected_box());
		obs_source_release(layout);
		const QString signature = state_signature(state);
		if (signature != playlist_signature) {
			const int selected = queue->currentRow();
			updating_ui = true;
			queue->clear();
			for (int i = 0; i < static_cast<int>(state.items.size()); i++) {
				const PlaylistItem &entry = state.items[static_cast<size_t>(i)];
				obs_source_t *source = resolve_item(entry);
				const QString actual_name = source ? QString::fromUtf8(obs_source_get_name(source))
								   : entry.source_name;
				const QString prefix = i == state.current ? QStringLiteral("▶  ")
									  : QStringLiteral("    ");
				auto *row = new QListWidgetItem(prefix + actual_name + QStringLiteral("  ·  ") +
									fit_mode_name(entry.fit_mode),
								queue);
				if (!source) {
					row->setText(prefix + QStringLiteral("⚠ ") + actual_name +
						     QStringLiteral("  ·  ") + fit_mode_name(entry.fit_mode));
					row->setForeground(QColor(230, 120, 80));
				}
				if (source)
					obs_source_release(source);
			}
			if (!state.items.empty())
				queue->setCurrentRow(std::clamp(selected, 0, static_cast<int>(state.items.size()) - 1));
			auto_advance->setChecked(state.auto_advance);
			loop->setChecked(state.loop);
			updating_ui = false;
			playlist_signature = signature;
		}
		set_available(true);
		update_selection_controls();
	}

	void set_available(bool enabled)
	{
		box_selector->setEnabled(enabled);
		source_picker->setEnabled(enabled);
		add_button->setEnabled(enabled && !source_picker->currentData().toString().isEmpty());
		queue->setEnabled(enabled);
		auto_advance->setEnabled(enabled);
		loop->setEnabled(enabled);
		warning->setVisible(enabled);
		if (!enabled) {
			status->setText(tr_text("Playlist.NoCurrent"));
			progress->setValue(0);
		}
	}

	void update_selection_controls()
	{
		obs_source_t *layout = selected_layout();
		const PlaylistState state = load_playlist(layout, selected_box());
		if (layout)
			obs_source_release(layout);
		const int row = queue->currentRow();
		const bool selected = row >= 0 && row < static_cast<int>(state.items.size());
		remove_button->setEnabled(selected);
		save_framing_button->setEnabled(selected);
		up_button->setEnabled(selected && row > 0);
		down_button->setEnabled(selected && row + 1 < static_cast<int>(state.items.size()));
		take_button->setEnabled(selected && !preview_shared);
		previous_button->setEnabled(!state.items.empty() && !preview_shared);
		next_button->setEnabled(!state.items.empty() && !preview_shared);
		duration->setEnabled(selected);
		if (selected) {
			updating_ui = true;
			duration->setValue(std::max(1, state.items[static_cast<size_t>(row)].duration_ms / 1000));
			updating_ui = false;
		}
	}

	void add_item()
	{
		obs_source_t *layout = selected_layout();
		if (!layout)
			return;
		const QString uuid = source_picker->currentData().toString();
		if (uuid.isEmpty()) {
			obs_source_release(layout);
			return;
		}
		PlaylistState state = load_playlist(layout, selected_box());
		PlaylistItem item = current_box_framing(layout, selected_box());
		item.source_uuid = uuid;
		obs_source_t *source = obs_get_source_by_uuid(uuid.toUtf8().constData());
		item.source_name = source ? QString::fromUtf8(obs_source_get_name(source))
					  : source_picker->currentText();
		if (source)
			obs_source_release(source);
		item.duration_ms = duration->value() * 1000;
		state.items.push_back(item);
		save_playlist(layout, selected_box(), state);
		obs_source_release(layout);
		playlist_signature.clear();
		refresh_playlist();
		queue->setCurrentRow(static_cast<int>(state.items.size()) - 1);
	}

	void save_selected_framing()
	{
		obs_source_t *layout = selected_layout();
		if (!layout)
			return;
		PlaylistState state = load_playlist(layout, selected_box());
		const int row = queue->currentRow();
		if (row >= 0 && row < static_cast<int>(state.items.size())) {
			const PlaylistItem framing = current_box_framing(layout, selected_box());
			PlaylistItem &item = state.items[static_cast<size_t>(row)];
			item.fit_mode = framing.fit_mode;
			item.zoom = framing.zoom;
			item.pan_x = framing.pan_x;
			item.pan_y = framing.pan_y;
			save_playlist(layout, selected_box(), state);
		}
		obs_source_release(layout);
		playlist_signature.clear();
		refresh_playlist();
	}

	void remove_item()
	{
		obs_source_t *layout = selected_layout();
		if (!layout)
			return;
		PlaylistState state = load_playlist(layout, selected_box());
		const int row = queue->currentRow();
		if (row < 0 || row >= static_cast<int>(state.items.size())) {
			obs_source_release(layout);
			return;
		}
		state.items.erase(state.items.begin() + row);
		if (state.current == row)
			state.current = -1;
		else if (state.current > row)
			state.current--;
		save_playlist(layout, selected_box(), state);
		obs_source_release(layout);
		reset_runtime();
		playlist_signature.clear();
		refresh_playlist();
		if (!state.items.empty())
			queue->setCurrentRow(std::min(row, static_cast<int>(state.items.size()) - 1));
	}

	void move_item(int direction)
	{
		obs_source_t *layout = selected_layout();
		if (!layout)
			return;
		PlaylistState state = load_playlist(layout, selected_box());
		const int row = queue->currentRow();
		const int target = row + direction;
		if (row < 0 || target < 0 || row >= static_cast<int>(state.items.size()) ||
		    target >= static_cast<int>(state.items.size())) {
			obs_source_release(layout);
			return;
		}
		std::swap(state.items[static_cast<size_t>(row)], state.items[static_cast<size_t>(target)]);
		if (state.current == row)
			state.current = target;
		else if (state.current == target)
			state.current = row;
		save_playlist(layout, selected_box(), state);
		obs_source_release(layout);
		playlist_signature.clear();
		refresh_playlist();
		queue->setCurrentRow(target);
	}

	void set_selected_duration(int milliseconds)
	{
		obs_source_t *layout = selected_layout();
		if (!layout)
			return;
		PlaylistState state = load_playlist(layout, selected_box());
		const int row = queue->currentRow();
		if (row >= 0 && row < static_cast<int>(state.items.size())) {
			state.items[static_cast<size_t>(row)].duration_ms = std::clamp(milliseconds, 500, 3600000);
			save_playlist(layout, selected_box(), state);
			if (row == state.current)
				reset_runtime();
		}
		obs_source_release(layout);
		playlist_signature.clear();
	}

	void set_option(bool is_auto, bool enabled)
	{
		obs_source_t *layout = selected_layout();
		if (!layout)
			return;
		PlaylistState state = load_playlist(layout, selected_box());
		if (is_auto)
			state.auto_advance = enabled;
		else
			state.loop = enabled;
		save_playlist(layout, selected_box(), state);
		obs_source_release(layout);
		if (is_auto)
			reset_runtime();
		playlist_signature.clear();
	}

	void take_selected()
	{
		const int row = queue->currentRow();
		if (row >= 0)
			take_index(row, false);
	}

	void navigate(int direction)
	{
		obs_source_t *layout = selected_layout();
		if (!layout)
			return;
		const PlaylistState state = load_playlist(layout, selected_box());
		obs_source_release(layout);
		if (state.items.empty())
			return;
		int target = state.current;
		if (target < 0)
			target = queue->currentRow() >= 0 ? queue->currentRow() : 0;
		else
			target = (target + direction + static_cast<int>(state.items.size())) %
				 static_cast<int>(state.items.size());
		take_index(target, false);
	}

	void take_index(int index, bool automatic)
	{
		if (preview_shared)
			return;
		obs_source_t *layout = selected_layout();
		if (!layout)
			return;
		take_index_on_layout(layout, selected_box(), index, automatic);
		obs_source_release(layout);
	}

	void take_index_on_layout(obs_source_t *layout, int box, int index, bool automatic)
	{
		PlaylistState state = load_playlist(layout, box);
		if (index < 0 || index >= static_cast<int>(state.items.size())) {
			return;
		}
		PlaylistItem &item = state.items[static_cast<size_t>(index)];
		obs_source_t *source = resolve_item(item);
		if (!source || source == layout) {
			if (source)
				obs_source_release(source);
			if (box == selected_box())
				status->setText(tr_text("Playlist.MissingSource").arg(item.source_name));
			return;
		}
		item.source_uuid = QString::fromUtf8(obs_source_get_uuid(source));
		item.source_name = QString::fromUtf8(obs_source_get_name(source));
		state.current = index;
		save_playlist(layout, box, state, &item);
		if (obs_source_get_output_flags(source) & OBS_SOURCE_CONTROLLABLE_MEDIA)
			obs_source_media_restart(source);
		blog(LOG_INFO, "[obs-box-layouts] playlist %s take layout='%s' box=%d item=%d source='%s'",
		     automatic ? "auto" : "manual", obs_source_get_name(layout), box + 1, index + 1,
		     obs_source_get_name(source));
		obs_source_release(source);
		if (box == selected_box() &&
		    QString::fromUtf8(obs_source_get_uuid(layout)) == layout_selector->currentData().toString()) {
			reset_runtime();
			playlist_signature.clear();
			refresh_playlist();
			queue->setCurrentRow(index);
		}
	}

	void tick_current()
	{
		obs_source_t *layout = selected_layout();
		if (!layout)
			return;
		const int box = selected_box();
		const PlaylistState state = load_playlist(layout, box);
		if (state.current < 0 || state.current >= static_cast<int>(state.items.size())) {
			obs_source_release(layout);
			status->setText(tr_text("Playlist.NoCurrent"));
			progress->setValue(0);
			return;
		}
		const PlaylistItem &item = state.items[static_cast<size_t>(state.current)];
		obs_source_t *source = resolve_item(item);
		if (!source) {
			obs_source_release(layout);
			status->setText(tr_text("Playlist.MissingSource").arg(item.source_name));
			progress->setValue(0);
			return;
		}
		if (assigned_box_source(layout, box) != QString::fromUtf8(obs_source_get_name(source))) {
			obs_source_release(source);
			obs_source_release(layout);
			reset_runtime();
			status->setText(tr_text("Playlist.SourceChanged"));
			progress->setValue(0);
			return;
		}

		const QString key = QString::fromUtf8(obs_source_get_uuid(layout)) + QLatin1Char('|') +
				    QString::number(box) + QLatin1Char('|') + QString::number(state.current) +
				    QLatin1Char('|') + QString::fromUtf8(obs_source_get_uuid(source));
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		if (key != runtime_key) {
			runtime_key = key;
			runtime_started = now;
			grace_until = now + 750;
			previous_state = obs_source_media_get_state(source);
			previous_time = std::max<int64_t>(obs_source_media_get_time(source), 0);
			runtime_finished = false;
			runtime_auto = state.auto_advance;
		} else if (state.auto_advance && !runtime_auto) {
			runtime_started = now;
			grace_until = now + 500;
			previous_state = obs_source_media_get_state(source);
			previous_time = std::max<int64_t>(obs_source_media_get_time(source), 0);
			runtime_finished = false;
		}
		runtime_auto = state.auto_advance;
		if (runtime_finished) {
			obs_source_release(source);
			obs_source_release(layout);
			status->setText(tr_text("Playlist.Completed"));
			progress->setValue(1000);
			return;
		}

		bool ended_now = false;
		const uint32_t flags = obs_source_get_output_flags(source);
		if (flags & OBS_SOURCE_CONTROLLABLE_MEDIA) {
			const enum obs_media_state media_state = obs_source_media_get_state(source);
			const int64_t total = std::max<int64_t>(obs_source_media_get_duration(source), 0);
			const int64_t current = std::clamp<int64_t>(obs_source_media_get_time(source), 0,
								    total > 0 ? total : INT64_MAX);
			const int64_t remaining = total > 0 ? std::max<int64_t>(total - current, 0) : 0;
			status->setText(total > 0 ? tr_text("Playlist.MediaStatus")
							    .arg(item.source_name, format_time(current),
								 format_time(total), format_time(remaining))
						  : tr_text("Playlist.MediaUnknown").arg(item.source_name));
			progress->setValue(total > 0 ? static_cast<int>(current * 1000 / total) : 0);
			const int64_t loop_window =
				total > 0 ? std::min<int64_t>(2000, std::max<int64_t>(total / 4, 250)) : 0;
			const bool loop_wrapped = media_state == OBS_MEDIA_STATE_PLAYING &&
						  previous_state == OBS_MEDIA_STATE_PLAYING && total > 0 &&
						  previous_time >= total - loop_window && current <= loop_window;
			const bool ended_state = media_state == OBS_MEDIA_STATE_ENDED &&
						 (previous_state == OBS_MEDIA_STATE_PLAYING ||
						  previous_state == OBS_MEDIA_STATE_BUFFERING);
			ended_now = now >= grace_until && (loop_wrapped || ended_state);
			previous_state = media_state;
			previous_time = current;
		} else {
			const int64_t total = std::max(item.duration_ms, 500);
			if (!state.auto_advance) {
				status->setText(
					tr_text("Playlist.StillReady").arg(item.source_name, format_time(total)));
				progress->setValue(0);
				obs_source_release(source);
				obs_source_release(layout);
				return;
			}
			const int64_t elapsed = std::clamp<int64_t>(now - runtime_started, 0, total);
			const int64_t remaining = std::max<int64_t>(total - elapsed, 0);
			status->setText(tr_text("Playlist.StillStatus").arg(item.source_name, format_time(remaining)));
			progress->setValue(static_cast<int>(elapsed * 1000 / total));
			ended_now = state.auto_advance && now >= grace_until && elapsed >= total;
		}
		obs_source_release(source);
		obs_source_release(layout);

		if (state.auto_advance && ended_now && !runtime_finished)
			advance_automatic(state);
	}

	void advance_automatic(const PlaylistState &state)
	{
		int target = state.current + 1;
		if (target >= static_cast<int>(state.items.size())) {
			if (state.loop)
				target = 0;
			else {
				if (state.current >= 0 && state.current < static_cast<int>(state.items.size())) {
					obs_source_t *source =
						resolve_item(state.items[static_cast<size_t>(state.current)]);
					if (source) {
						if (obs_source_get_output_flags(source) & OBS_SOURCE_CONTROLLABLE_MEDIA)
							obs_source_media_stop(source);
						obs_source_release(source);
					}
				}
				runtime_finished = true;
				status->setText(tr_text("Playlist.Completed"));
				progress->setValue(1000);
				return;
			}
		}
		take_index(target, true);
	}

	void reset_runtime()
	{
		runtime_key.clear();
		runtime_started = 0;
		grace_until = 0;
		previous_state = OBS_MEDIA_STATE_NONE;
		previous_time = 0;
		runtime_finished = false;
		runtime_auto = false;
	}

	QLabel *program_label = nullptr;
	QComboBox *target_selector = nullptr;
	QComboBox *layout_selector = nullptr;
	QComboBox *box_selector = nullptr;
	QLabel *warning = nullptr;
	QComboBox *source_picker = nullptr;
	QPushButton *add_button = nullptr;
	QListWidget *queue = nullptr;
	QPushButton *remove_button = nullptr;
	QPushButton *save_framing_button = nullptr;
	QPushButton *up_button = nullptr;
	QPushButton *down_button = nullptr;
	QSpinBox *duration = nullptr;
	QPushButton *previous_button = nullptr;
	QPushButton *take_button = nullptr;
	QPushButton *next_button = nullptr;
	QCheckBox *auto_advance = nullptr;
	QCheckBox *loop = nullptr;
	QLabel *status = nullptr;
	QProgressBar *progress = nullptr;
	QTimer *timer = nullptr;
	QString program_uuid;
	QString preferred_program_layout_uuid;
	QString layout_signature;
	QString source_signature;
	QString playlist_signature;
	bool updating_ui = false;
	bool studio_mode_known = false;
	bool preview_shared = false;

	QString runtime_key;
	qint64 runtime_started = 0;
	qint64 grace_until = 0;
	enum obs_media_state previous_state = OBS_MEDIA_STATE_NONE;
	int64_t previous_time = 0;
	bool runtime_finished = false;
	bool runtime_auto = false;
};

PlaylistDock *playlist = nullptr;

struct HotkeyAction {
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
	int box = 0;
	int action = 0;
	std::string name;
};

constexpr size_t HOTKEY_COUNT = MAX_BOXES * (HOTKEY_DIRECT_ITEMS + 2);
std::array<HotkeyAction, HOTKEY_COUNT> hotkeys;

void playlist_hotkey_callback(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || !playlist || !data)
		return;
	const auto *hotkey = static_cast<HotkeyAction *>(data);
	const int box = hotkey->box;
	const int action = hotkey->action;
	QMetaObject::invokeMethod(
		playlist,
		[box, action]() {
			if (playlist)
				playlist->trigger_hotkey(box, action);
		},
		Qt::QueuedConnection);
}

void playlist_hotkeys_save(obs_data_t *save_data, bool saving, void *)
{
	constexpr const char *settings_key = "obs-box-layouts-playlist-hotkeys";
	if (saving) {
		obs_data_t *saved = obs_data_create();
		for (const HotkeyAction &hotkey : hotkeys) {
			if (hotkey.id == OBS_INVALID_HOTKEY_ID)
				continue;
			obs_data_array_t *bindings = obs_hotkey_save(hotkey.id);
			obs_data_set_array(saved, hotkey.name.c_str(), bindings);
			obs_data_array_release(bindings);
		}
		obs_data_set_obj(save_data, settings_key, saved);
		obs_data_release(saved);
		return;
	}

	obs_data_t *saved = obs_data_get_obj(save_data, settings_key);
	if (!saved)
		return;
	for (const HotkeyAction &hotkey : hotkeys) {
		if (hotkey.id == OBS_INVALID_HOTKEY_ID)
			continue;
		obs_data_array_t *bindings = obs_data_get_array(saved, hotkey.name.c_str());
		if (bindings) {
			obs_hotkey_load(hotkey.id, bindings);
			obs_data_array_release(bindings);
		}
	}
	obs_data_release(saved);
}

void register_playlist_hotkeys()
{
	size_t index = 0;
	auto register_action = [&index](int box, int action, const QString &description) {
		HotkeyAction &hotkey = hotkeys[index++];
		hotkey.box = box;
		hotkey.action = action;
		char name[96];
		if (action == HOTKEY_ACTION_PREVIOUS)
			snprintf(name, sizeof(name), "obs_box_layouts_box_%d_previous", box + 1);
		else if (action == HOTKEY_ACTION_NEXT)
			snprintf(name, sizeof(name), "obs_box_layouts_box_%d_next", box + 1);
		else
			snprintf(name, sizeof(name), "obs_box_layouts_box_%d_take_%d", box + 1, action + 1);
		hotkey.name = name;
		const QByteArray text = description.toUtf8();
		hotkey.id = obs_hotkey_register_frontend(hotkey.name.c_str(), text.constData(),
							 playlist_hotkey_callback, &hotkey);
	};

	for (int box = 0; box < MAX_BOXES; box++) {
		register_action(box, HOTKEY_ACTION_PREVIOUS, tr_text("Hotkey.Previous").arg(box + 1));
		register_action(box, HOTKEY_ACTION_NEXT, tr_text("Hotkey.Next").arg(box + 1));
		for (int item = 0; item < HOTKEY_DIRECT_ITEMS; item++)
			register_action(box, item, tr_text("Hotkey.Take").arg(box + 1).arg(item + 1));
	}
	obs_frontend_add_save_callback(playlist_hotkeys_save, nullptr);
	blog(LOG_INFO, "[obs-box-layouts] registered %zu playlist hotkeys", index);
}

void unregister_playlist_hotkeys()
{
	obs_frontend_remove_save_callback(playlist_hotkeys_save, nullptr);
	for (HotkeyAction &hotkey : hotkeys) {
		if (hotkey.id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(hotkey.id);
		hotkey.id = OBS_INVALID_HOTKEY_ID;
	}
}

} // namespace

extern "C" bool playlist_dock_init(void)
{
	if (playlist)
		return true;
	playlist = new PlaylistDock;
	if (!obs_frontend_add_dock_by_id(DOCK_ID, obs_module_text("Playlist.DockTitle"), playlist)) {
		delete playlist;
		playlist = nullptr;
		return false;
	}
	register_playlist_hotkeys();
	QTimer::singleShot(0, playlist, []() {
		if (playlist && playlist->parentWidget())
			playlist->parentWidget()->show();
	});
	blog(LOG_INFO, "[obs-box-layouts] playlist dock registered");
	return true;
}

extern "C" void playlist_dock_shutdown(void)
{
	if (!playlist)
		return;
	unregister_playlist_hotkeys();
	obs_frontend_remove_dock(DOCK_ID);
	playlist = nullptr;
}
