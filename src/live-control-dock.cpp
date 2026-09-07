#include "live-control-dock.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QtCore/QByteArray>
#include <QtCore/QPointF>
#include <QtCore/QRectF>
#include <QtCore/QSignalBlocker>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPen>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr const char *DOCK_ID = "obs-box-layouts-live-control";
constexpr int MAX_BOXES = 6;
constexpr int PRESET_CUSTOM = 8;

enum ResizeEdges {
	ResizeNone = 0,
	ResizeLeft = 1 << 0,
	ResizeRight = 1 << 1,
	ResizeTop = 1 << 2,
	ResizeBottom = 1 << 3,
};

struct EditorBox {
	QRectF rect;
	double zoom = 1.0;
	double pan_x = 0.0;
	double pan_y = 0.0;
	int z_index = 0;
	bool lock_aspect = false;
	QString source_name;
};

struct EditorLayout {
	int width = 1920;
	int height = 1080;
	int preset = 0;
	int count = 1;
	double gap = 0.0;
	std::array<EditorBox, MAX_BOXES> boxes;
};

struct UndoEntry {
	QString source_uuid;
	std::string settings_json;
};

QString tr_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
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

void set_box_rect(EditorLayout &layout, int index, double x, double y, double width, double height)
{
	layout.boxes[index].rect = QRectF(x, y, std::max(width, 1.0), std::max(height, 1.0));
}

void calculate_preset_rects(EditorLayout &layout)
{
	const double w = layout.width;
	const double h = layout.height;
	const double g = std::max(layout.gap, 0.0);
	const double half_w = (w - g) * 0.5;
	const double half_h = (h - g) * 0.5;
	const double third_w = (w - 2.0 * g) / 3.0;

	switch (layout.preset) {
	case 0:
		set_box_rect(layout, 0, 0, 0, w, h);
		break;
	case 1:
		set_box_rect(layout, 0, 0, 0, half_w, h);
		set_box_rect(layout, 1, half_w + g, 0, half_w, h);
		break;
	case 2:
		set_box_rect(layout, 0, 0, 0, w, half_h);
		set_box_rect(layout, 1, 0, half_h + g, w, half_h);
		break;
	case 3:
		for (int i = 0; i < 3; i++)
			set_box_rect(layout, i, i * (third_w + g), 0, third_w, h);
		break;
	case 4: {
		const double hero_w = (w - g) * 0.666667;
		const double side_w = w - hero_w - g;
		set_box_rect(layout, 0, 0, 0, hero_w, h);
		set_box_rect(layout, 1, hero_w + g, 0, side_w, half_h);
		set_box_rect(layout, 2, hero_w + g, half_h + g, side_w, half_h);
		break;
	}
	case 5:
		set_box_rect(layout, 0, 0, 0, half_w, half_h);
		set_box_rect(layout, 1, half_w + g, 0, half_w, half_h);
		set_box_rect(layout, 2, 0, half_h + g, half_w, half_h);
		set_box_rect(layout, 3, half_w + g, half_h + g, half_w, half_h);
		break;
	case 6: {
		const double hero_h = (h - g) * 0.666667;
		set_box_rect(layout, 0, 0, 0, w, hero_h);
		for (int i = 0; i < 3; i++)
			set_box_rect(layout, i + 1, i * (third_w + g), hero_h + g, third_w, h - hero_h - g);
		break;
	}
	case 7:
	default:
		for (int row = 0; row < 2; row++)
			for (int column = 0; column < 3; column++)
				set_box_rect(layout, row * 3 + column, column * (third_w + g),
					     row * (half_h + g), third_w, half_h);
		break;
	}
}

EditorLayout load_layout(obs_source_t *source)
{
	EditorLayout layout;
	if (!source)
		return layout;
	obs_data_t *settings = obs_source_get_settings(source);
	layout.width = std::max<int>(static_cast<int>(obs_data_get_int(settings, "width")), 1);
	layout.height = std::max<int>(static_cast<int>(obs_data_get_int(settings, "height")), 1);
	layout.preset = static_cast<int>(obs_data_get_int(settings, "preset"));
	layout.gap = obs_data_get_double(settings, "gap");
	layout.count = layout.preset == PRESET_CUSTOM
			       ? std::clamp<int>(static_cast<int>(obs_data_get_int(settings, "custom_box_count")), 1,
						 MAX_BOXES)
			       : preset_box_count(layout.preset);

	char key[64];
	for (int i = 0; i < MAX_BOXES; i++) {
		snprintf(key, sizeof(key), "box_%d_source", i + 1);
		layout.boxes[i].source_name = QString::fromUtf8(obs_data_get_string(settings, key));
		snprintf(key, sizeof(key), "box_%d_zoom", i + 1);
		layout.boxes[i].zoom = obs_data_get_double(settings, key);
		snprintf(key, sizeof(key), "box_%d_pan_x", i + 1);
		layout.boxes[i].pan_x = obs_data_get_double(settings, key);
		snprintf(key, sizeof(key), "box_%d_pan_y", i + 1);
		layout.boxes[i].pan_y = obs_data_get_double(settings, key);
		snprintf(key, sizeof(key), "box_%d_z_index", i + 1);
		layout.boxes[i].z_index = static_cast<int>(obs_data_get_int(settings, key));
		snprintf(key, sizeof(key), "box_%d_lock_aspect", i + 1);
		layout.boxes[i].lock_aspect = obs_data_get_bool(settings, key);
		if (layout.preset == PRESET_CUSTOM) {
			snprintf(key, sizeof(key), "box_%d_custom_x", i + 1);
			const double x = obs_data_get_double(settings, key);
			snprintf(key, sizeof(key), "box_%d_custom_y", i + 1);
			const double y = obs_data_get_double(settings, key);
			snprintf(key, sizeof(key), "box_%d_custom_width", i + 1);
			const double width = obs_data_get_double(settings, key);
			snprintf(key, sizeof(key), "box_%d_custom_height", i + 1);
			const double height = obs_data_get_double(settings, key);
			set_box_rect(layout, i, x, y, width, height);
		}
	}
	if (layout.preset != PRESET_CUSTOM)
		calculate_preset_rects(layout);
	obs_data_release(settings);
	return layout;
}

void write_custom_rects(obs_data_t *settings, const EditorLayout &layout)
{
	obs_data_set_int(settings, "preset", PRESET_CUSTOM);
	obs_data_set_int(settings, "custom_box_count", layout.count);
	char key[64];
	for (int i = 0; i < layout.count; i++) {
		const QRectF rect = layout.boxes[i].rect;
		snprintf(key, sizeof(key), "box_%d_custom_x", i + 1);
		obs_data_set_double(settings, key, rect.x());
		snprintf(key, sizeof(key), "box_%d_custom_y", i + 1);
		obs_data_set_double(settings, key, rect.y());
		snprintf(key, sizeof(key), "box_%d_custom_width", i + 1);
		obs_data_set_double(settings, key, rect.width());
		snprintf(key, sizeof(key), "box_%d_custom_height", i + 1);
		obs_data_set_double(settings, key, rect.height());
	}
}

void update_geometry(obs_source_t *source, int index, const QRectF &rect, const EditorLayout &baseline)
{
	if (!source || index < 0 || index >= baseline.count)
		return;
	obs_data_t *settings = obs_source_get_settings(source);
	if (obs_data_get_int(settings, "preset") != PRESET_CUSTOM)
		write_custom_rects(settings, baseline);
	char key[64];
	snprintf(key, sizeof(key), "box_%d_custom_x", index + 1);
	obs_data_set_double(settings, key, rect.x());
	snprintf(key, sizeof(key), "box_%d_custom_y", index + 1);
	obs_data_set_double(settings, key, rect.y());
	snprintf(key, sizeof(key), "box_%d_custom_width", index + 1);
	obs_data_set_double(settings, key, rect.width());
	snprintf(key, sizeof(key), "box_%d_custom_height", index + 1);
	obs_data_set_double(settings, key, rect.height());
	obs_source_update(source, settings);
	obs_data_release(settings);
}

void update_content(obs_source_t *source, int index, double zoom, double pan_x, double pan_y)
{
	if (!source || index < 0 || index >= MAX_BOXES)
		return;
	obs_data_t *settings = obs_source_get_settings(source);
	char key[64];
	snprintf(key, sizeof(key), "box_%d_zoom", index + 1);
	obs_data_set_double(settings, key, std::clamp(zoom, 1.0, 4.0));
	snprintf(key, sizeof(key), "box_%d_pan_x", index + 1);
	obs_data_set_double(settings, key, std::clamp(pan_x, -100.0, 100.0));
	snprintf(key, sizeof(key), "box_%d_pan_y", index + 1);
	obs_data_set_double(settings, key, std::clamp(pan_y, -100.0, 100.0));
	obs_source_update(source, settings);
	obs_data_release(settings);
}

class LayoutCanvas final : public QWidget {
public:
	LayoutCanvas()
	{
		setMinimumHeight(230);
		setMouseTracking(true);
		setFocusPolicy(Qt::StrongFocus);
	}

	~LayoutCanvas() override
	{
		if (weak_source)
			obs_weak_source_release(weak_source);
	}

	void set_source(obs_source_t *source)
	{
		obs_source_t *current = weak_source ? obs_weak_source_get_source(weak_source) : nullptr;
		const bool same = current == source;
		if (current)
			obs_source_release(current);
		if (same)
			return;
		if (weak_source)
			obs_weak_source_release(weak_source);
		weak_source = source ? obs_source_get_weak_source(source) : nullptr;
		selected_box = 0;
		dragging = false;
		update();
		if (selection_changed)
			selection_changed(selected_box);
	}

	void set_locked(bool value)
	{
		locked = value;
		update();
	}

	int selection() const { return selected_box; }
	bool has_source() const { return weak_source != nullptr; }

	std::function<void()> before_edit;
	std::function<void(int)> selection_changed;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.fillRect(rect(), QColor(20, 22, 26));
		obs_source_t *source = strong_source();
		if (!source) {
			painter.setPen(QColor(170, 170, 170));
			painter.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, tr_text("Live.NoLayout"));
			return;
		}

		const EditorLayout layout = load_layout(source);
		obs_source_release(source);
		const QRectF area = display_area(layout);
		painter.fillRect(area, QColor(7, 8, 10));
		painter.setClipRect(area);
		std::array<int, MAX_BOXES> order{};
		for (int i = 0; i < layout.count; i++)
			order[i] = i;
		std::stable_sort(order.begin(), order.begin() + layout.count, [&layout](int a, int b) {
			return layout.boxes[a].z_index < layout.boxes[b].z_index;
		});
		for (int position = 0; position < layout.count; position++) {
			const int index = order[position];
			const QRectF box = canvas_to_widget(layout.boxes[index].rect, layout);
			const bool selected = index == selected_box;
			painter.setBrush(selected ? QColor(35, 105, 155, 145) : QColor(75, 78, 84, 150));
			painter.setPen(QPen(selected ? QColor(75, 190, 255) : QColor(180, 184, 190), selected ? 3 : 1));
			painter.drawRoundedRect(box, 4, 4);
			painter.setPen(Qt::white);
			QString name = layout.boxes[index].source_name;
			if (name.isEmpty())
				name = tr_text("Live.EmptyBox");
			painter.drawText(box.adjusted(7, 5, -7, -5), Qt::AlignCenter | Qt::TextWordWrap,
					 QStringLiteral("BOX %1\n%2").arg(index + 1).arg(name));
			if (selected) {
				painter.setBrush(QColor(75, 190, 255));
				for (const QPointF &handle : {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()})
					painter.drawRect(QRectF(handle.x() - 4, handle.y() - 4, 8, 8));
			}
		}
		painter.setClipping(false);
		painter.setPen(QPen(locked ? QColor(150, 150, 150) : QColor(235, 65, 75), locked ? 1 : 3));
		painter.drawRect(area);
	}

	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() != Qt::LeftButton)
			return;
		obs_source_t *source = strong_source();
		if (!source)
			return;
		const EditorLayout layout = load_layout(source);
		obs_source_release(source);
		const QPointF canvas_point = widget_to_canvas(event->position(), layout);
		const int hit = hit_test(canvas_point, layout);
		if (hit < 0)
			return;
		selected_box = hit;
		if (selection_changed)
			selection_changed(selected_box);
		update();
		if (locked)
			return;

		baseline = layout;
		drag_start = canvas_point;
		start_rect = layout.boxes[hit].rect;
		start_pan_x = layout.boxes[hit].pan_x;
		start_pan_y = layout.boxes[hit].pan_y;
		drag_content = event->modifiers().testFlag(Qt::ControlModifier) ||
			event->modifiers().testFlag(Qt::MetaModifier);
		drag_edges = detect_edges(event->position(), layout.boxes[hit].rect, layout);
		if (event->modifiers().testFlag(Qt::ShiftModifier))
			drag_edges = ResizeRight | ResizeBottom;
		dragging = true;
		if (before_edit)
			before_edit();
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (!dragging || locked)
			return;
		obs_source_t *source = strong_source();
		if (!source)
			return;
		const QPointF point = widget_to_canvas(event->position(), baseline);
		const QPointF delta = point - drag_start;
		if (drag_content) {
			const double pan_x = start_pan_x - 200.0 * delta.x() / std::max(start_rect.width(), 1.0);
			const double pan_y = start_pan_y - 200.0 * delta.y() / std::max(start_rect.height(), 1.0);
			update_content(source, selected_box, baseline.boxes[selected_box].zoom, pan_x, pan_y);
		} else {
			QRectF changed = resized_rect(delta, baseline);
			update_geometry(source, selected_box, changed, baseline);
		}
		obs_source_release(source);
		update();
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton)
			dragging = false;
	}

	void wheelEvent(QWheelEvent *event) override
	{
		if (locked)
			return;
		obs_source_t *source = strong_source();
		if (!source)
			return;
		const EditorLayout layout = load_layout(source);
		const int hit = hit_test(widget_to_canvas(event->position(), layout), layout);
		if (hit >= 0) {
			selected_box = hit;
			if (before_edit)
				before_edit();
			const double amount = event->angleDelta().y() >= 0 ? 0.1 : -0.1;
			update_content(source, hit, layout.boxes[hit].zoom + amount, layout.boxes[hit].pan_x,
				       layout.boxes[hit].pan_y);
			if (selection_changed)
				selection_changed(selected_box);
		}
		obs_source_release(source);
		update();
	}

	void mouseDoubleClickEvent(QMouseEvent *event) override
	{
		if (locked || event->button() != Qt::LeftButton)
			return;
		obs_source_t *source = strong_source();
		if (!source)
			return;
		const EditorLayout layout = load_layout(source);
		const int hit = hit_test(widget_to_canvas(event->position(), layout), layout);
		if (hit >= 0) {
			if (before_edit)
				before_edit();
			selected_box = hit;
			update_content(source, hit, 1.0, 0.0, 0.0);
		}
		obs_source_release(source);
		update();
	}

private:
	obs_source_t *strong_source() const
	{
		return weak_source ? obs_weak_source_get_source(weak_source) : nullptr;
	}

	QRectF display_area(const EditorLayout &layout) const
	{
		const QRectF available = QRectF(rect()).adjusted(10, 10, -10, -10);
		const double scale = std::min(available.width() / layout.width, available.height() / layout.height);
		const QSizeF size(layout.width * scale, layout.height * scale);
		return QRectF(available.center().x() - size.width() * 0.5, available.center().y() - size.height() * 0.5,
			      size.width(), size.height());
	}

	QRectF canvas_to_widget(const QRectF &canvas, const EditorLayout &layout) const
	{
		const QRectF area = display_area(layout);
		return QRectF(area.x() + canvas.x() * area.width() / layout.width,
			      area.y() + canvas.y() * area.height() / layout.height,
			      canvas.width() * area.width() / layout.width,
			      canvas.height() * area.height() / layout.height);
	}

	QPointF widget_to_canvas(const QPointF &point, const EditorLayout &layout) const
	{
		const QRectF area = display_area(layout);
		return QPointF((point.x() - area.x()) * layout.width / area.width(),
			       (point.y() - area.y()) * layout.height / area.height());
	}

	int hit_test(const QPointF &point, const EditorLayout &layout) const
	{
		std::array<int, MAX_BOXES> order{};
		for (int i = 0; i < layout.count; i++)
			order[i] = i;
		std::stable_sort(order.begin(), order.begin() + layout.count, [&layout](int a, int b) {
			return layout.boxes[a].z_index < layout.boxes[b].z_index;
		});
		for (int i = layout.count - 1; i >= 0; i--)
			if (layout.boxes[order[i]].rect.contains(point))
				return order[i];
		return -1;
	}

	int detect_edges(const QPointF &widget_point, const QRectF &canvas_rect, const EditorLayout &layout) const
	{
		const QRectF box = canvas_to_widget(canvas_rect, layout);
		const double threshold = 10.0;
		int edges = ResizeNone;
		if (std::abs(widget_point.x() - box.left()) <= threshold)
			edges |= ResizeLeft;
		else if (std::abs(widget_point.x() - box.right()) <= threshold)
			edges |= ResizeRight;
		if (std::abs(widget_point.y() - box.top()) <= threshold)
			edges |= ResizeTop;
		else if (std::abs(widget_point.y() - box.bottom()) <= threshold)
			edges |= ResizeBottom;
		return edges;
	}

	QRectF resized_rect(const QPointF &delta, const EditorLayout &layout) const
	{
		const double minimum = 40.0;
		if (drag_edges == ResizeNone) {
			const double x = std::clamp(start_rect.x() + delta.x(), 0.0,
						    std::max(0.0, static_cast<double>(layout.width) - start_rect.width()));
			const double y = std::clamp(start_rect.y() + delta.y(), 0.0,
						    std::max(0.0, static_cast<double>(layout.height) - start_rect.height()));
			return QRectF(x, y, start_rect.width(), start_rect.height());
		}
		double left = start_rect.left();
		double right = start_rect.right();
		double top = start_rect.top();
		double bottom = start_rect.bottom();
		if (drag_edges & ResizeLeft)
			left = std::clamp(start_rect.left() + delta.x(), 0.0, right - minimum);
		if (drag_edges & ResizeRight)
			right = std::clamp(start_rect.right() + delta.x(), left + minimum,
					   static_cast<double>(layout.width));
		if (drag_edges & ResizeTop)
			top = std::clamp(start_rect.top() + delta.y(), 0.0, bottom - minimum);
		if (drag_edges & ResizeBottom)
			bottom = std::clamp(start_rect.bottom() + delta.y(), top + minimum,
					    static_cast<double>(layout.height));
		const bool horizontal = (drag_edges & (ResizeLeft | ResizeRight)) != 0;
		const bool vertical = (drag_edges & (ResizeTop | ResizeBottom)) != 0;
		if (baseline.boxes[selected_box].lock_aspect && horizontal && vertical && start_rect.height() > 0.0) {
			const double aspect = start_rect.width() / start_rect.height();
			double width = right - left;
			double height = bottom - top;
			if (std::abs(width - start_rect.width()) > std::abs(height - start_rect.height()) * aspect)
				height = width / aspect;
			else
				width = height * aspect;
			if (drag_edges & ResizeLeft)
				left = right - width;
			else
				right = left + width;
			if (drag_edges & ResizeTop)
				top = bottom - height;
			else
				bottom = top + height;
			left = std::max(0.0, left);
			top = std::max(0.0, top);
			right = std::min(static_cast<double>(layout.width), right);
			bottom = std::min(static_cast<double>(layout.height), bottom);
		}
		return QRectF(QPointF(left, top), QPointF(right, bottom));
	}

	obs_weak_source_t *weak_source = nullptr;
	bool locked = true;
	bool dragging = false;
	bool drag_content = false;
	int selected_box = 0;
	int drag_edges = ResizeNone;
	QPointF drag_start;
	QRectF start_rect;
	double start_pan_x = 0.0;
	double start_pan_y = 0.0;
	EditorLayout baseline;
};

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

class LiveControl final : public QWidget {
public:
	LiveControl()
	{
		setMinimumWidth(370);
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(8, 8, 8, 8);
		root->setSpacing(7);

		program_label = new QLabel(tr_text("Live.NoLayout"), this);
		QFont heading = program_label->font();
		heading.setBold(true);
		program_label->setFont(heading);
		root->addWidget(program_label);

		layout_selector = new QComboBox(this);
		root->addWidget(layout_selector);
		unlock = new QCheckBox(tr_text("Live.Unlock"), this);
		root->addWidget(unlock);
		lock_status = new QLabel(this);
		lock_status->setAlignment(Qt::AlignCenter);
		root->addWidget(lock_status);

		canvas = new LayoutCanvas;
		root->addWidget(canvas, 1);
		auto *help = new QLabel(tr_text("Live.Help"), this);
		help->setWordWrap(true);
		help->setStyleSheet(QStringLiteral("color:#aeb4bd;font-size:11px;"));
		root->addWidget(help);
		selected_label = new QLabel(this);
		root->addWidget(selected_label);

		auto *nudge = new QHBoxLayout;
		nudge->addWidget(new QLabel(tr_text("Live.Nudge"), this));
		step_selector = new QComboBox(this);
		step_selector->addItem(QStringLiteral("1 px"), 1);
		step_selector->addItem(QStringLiteral("10 px"), 10);
		nudge->addWidget(step_selector);
		int button_index = 0;
		for (const auto &button : {std::pair<const char *, int>{"←", 0}, {"→", 1}, {"↑", 2}, {"↓", 3}}) {
			auto *control = new QPushButton(QString::fromUtf8(button.first), this);
			nudge_buttons[button_index++] = control;
			control->setMaximumWidth(42);
			connect(control, &QPushButton::clicked, this, [this, direction = button.second]() {
				nudge_selected(direction);
			});
			nudge->addWidget(control);
		}
		root->addLayout(nudge);

		auto *content_grid = new QGridLayout;
		zoom = make_slider(100, 400);
		pan_x = make_slider(-100, 100);
		pan_y = make_slider(-100, 100);
		content_grid->addWidget(new QLabel(tr_text("Live.Zoom"), this), 0, 0);
		content_grid->addWidget(zoom, 0, 1);
		content_grid->addWidget(new QLabel(tr_text("Live.PanX"), this), 1, 0);
		content_grid->addWidget(pan_x, 1, 1);
		content_grid->addWidget(new QLabel(tr_text("Live.PanY"), this), 2, 0);
		content_grid->addWidget(pan_y, 2, 1);
		root->addLayout(content_grid);

		auto *actions = new QHBoxLayout;
		undo = new QPushButton(tr_text("Live.Undo"), this);
		reset_content = new QPushButton(tr_text("Live.ResetContent"), this);
		actions->addWidget(undo);
		actions->addWidget(reset_content);
		root->addLayout(actions);

		connect(layout_selector, &QComboBox::currentIndexChanged, this, [this](int) {
			unlock->setChecked(false);
			refresh_selected_source();
		});
		connect(unlock, &QCheckBox::toggled, this, [this](bool checked) {
			canvas->set_locked(!checked);
			update_lock_ui();
		});
		canvas->before_edit = [this]() { push_undo(); };
		canvas->selection_changed = [this](int) { refresh_controls(); };
		connect(undo, &QPushButton::clicked, this, [this]() { undo_last(); });
		connect(reset_content, &QPushButton::clicked, this, [this]() { reset_selected_content(); });
		connect_slider(zoom, 0);
		connect_slider(pan_x, 1);
		connect_slider(pan_y, 2);

		timer = new QTimer(this);
		timer->setInterval(250);
		connect(timer, &QTimer::timeout, this, [this]() { refresh_program(); });
		timer->start();
		update_lock_ui();
		refresh_program();
	}

private:
	QSlider *make_slider(int minimum, int maximum)
	{
		auto *slider = new QSlider(Qt::Horizontal, this);
		slider->setRange(minimum, maximum);
		return slider;
	}

	void connect_slider(QSlider *slider, int kind)
	{
		connect(slider, &QSlider::sliderPressed, this, [this]() { push_undo(); });
		connect(slider, &QSlider::valueChanged, this, [this, slider, kind](int value) {
			if (!slider->isSliderDown() || !unlock->isChecked())
				return;
			obs_source_t *source = selected_source();
			if (!source)
				return;
			const EditorLayout layout = load_layout(source);
			const int index = canvas->selection();
			if (index < layout.count) {
				double next_zoom = layout.boxes[index].zoom;
				double next_pan_x = layout.boxes[index].pan_x;
				double next_pan_y = layout.boxes[index].pan_y;
				if (kind == 0)
					next_zoom = value / 100.0;
				else if (kind == 1)
					next_pan_x = value;
				else
					next_pan_y = value;
				update_content(source, index, next_zoom, next_pan_x, next_pan_y);
			}
			obs_source_release(source);
			canvas->update();
		});
	}

	void refresh_program()
	{
		LayoutScan scan;
		obs_source_t *program = obs_frontend_get_current_scene();
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
			unlock->setChecked(false);
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
			layout_signature = signature;
			refresh_selected_source();
		}

		program_label->setText(scan.layouts.empty() ? tr_text("Live.NoLayout")
							      : tr_text("Live.ProgramScene").arg(program_name));
		layout_selector->setVisible(scan.layouts.size() > 1);
		refresh_controls();
		for (obs_source_t *layout : scan.layouts)
			obs_source_release(layout);
	}

	void refresh_selected_source()
	{
		obs_source_t *source = selected_source();
		if (!source)
			unlock->setChecked(false);
		canvas->set_source(source);
		if (source)
			obs_source_release(source);
		update_lock_ui();
	}

	obs_source_t *selected_source() const
	{
		const QByteArray uuid = layout_selector->currentData().toString().toUtf8();
		return uuid.isEmpty() ? nullptr : obs_get_source_by_uuid(uuid.constData());
	}

	void refresh_controls()
	{
		obs_source_t *source = selected_source();
		if (!source) {
			selected_label->setText(tr_text("Live.NoBox"));
			set_controls_enabled(false);
			return;
		}
		const EditorLayout layout = load_layout(source);
		const int index = std::clamp(canvas->selection(), 0, layout.count - 1);
		const EditorBox &box = layout.boxes[index];
		selected_label->setText(tr_text("Live.SelectedBox")
						.arg(index + 1)
						.arg(qRound(box.rect.x()))
						.arg(qRound(box.rect.y()))
						.arg(qRound(box.rect.width()))
						.arg(qRound(box.rect.height())));
		if (!zoom->isSliderDown()) {
			const QSignalBlocker blocker(zoom);
			zoom->setValue(qRound(box.zoom * 100.0));
		}
		if (!pan_x->isSliderDown()) {
			const QSignalBlocker blocker(pan_x);
			pan_x->setValue(qRound(box.pan_x));
		}
		if (!pan_y->isSliderDown()) {
			const QSignalBlocker blocker(pan_y);
			pan_y->setValue(qRound(box.pan_y));
		}
		obs_source_release(source);
		set_controls_enabled(unlock->isChecked());
		canvas->update();
	}

	void set_controls_enabled(bool enabled)
	{
		step_selector->setEnabled(enabled);
		for (QPushButton *button : nudge_buttons)
			button->setEnabled(enabled);
		zoom->setEnabled(enabled);
		pan_x->setEnabled(enabled);
		pan_y->setEnabled(enabled);
		reset_content->setEnabled(enabled);
		undo->setEnabled(enabled && !undo_stack.empty());
	}

	void update_lock_ui()
	{
		const bool has_source = canvas->has_source();
		unlock->setEnabled(has_source);
		const bool editing = unlock->isChecked() && has_source;
		lock_status->setText(editing ? tr_text("Live.OnAirWarning") : tr_text("Live.Locked"));
		lock_status->setStyleSheet(editing ? QStringLiteral("background:#9f2530;color:white;padding:5px;font-weight:bold;")
							 : QStringLiteral("background:#3d434b;color:white;padding:5px;"));
		set_controls_enabled(editing);
	}

	void push_undo()
	{
		if (!unlock->isChecked())
			return;
		obs_source_t *source = selected_source();
		if (!source)
			return;
		obs_data_t *settings = obs_source_get_settings(source);
		const char *json = obs_data_get_json(settings);
		if (json) {
			undo_stack.push_back({QString::fromUtf8(obs_source_get_uuid(source)), json});
			if (undo_stack.size() > 30)
				undo_stack.erase(undo_stack.begin());
		}
		obs_data_release(settings);
		obs_source_release(source);
		undo->setEnabled(!undo_stack.empty());
	}

	void undo_last()
	{
		if (!unlock->isChecked())
			return;
		obs_source_t *source = selected_source();
		if (!source)
			return;
		const QString uuid = QString::fromUtf8(obs_source_get_uuid(source));
		for (auto entry = undo_stack.rbegin(); entry != undo_stack.rend(); ++entry) {
			if (entry->source_uuid != uuid)
				continue;
			obs_data_t *settings = obs_data_create_from_json(entry->settings_json.c_str());
			if (settings) {
				obs_source_update(source, settings);
				obs_data_release(settings);
			}
			undo_stack.erase(std::next(entry).base());
			break;
		}
		obs_source_release(source);
		undo->setEnabled(!undo_stack.empty());
		canvas->update();
		refresh_controls();
	}

	void nudge_selected(int direction)
	{
		if (!unlock->isChecked())
			return;
		obs_source_t *source = selected_source();
		if (!source)
			return;
		const EditorLayout layout = load_layout(source);
		const int index = canvas->selection();
		if (index < layout.count) {
			push_undo();
			QRectF rect = layout.boxes[index].rect;
			const double step = step_selector->currentData().toInt();
			if (direction == 0)
				rect.moveLeft(std::max(0.0, rect.left() - step));
			else if (direction == 1)
				rect.moveLeft(std::min(layout.width - rect.width(), rect.left() + step));
			else if (direction == 2)
				rect.moveTop(std::max(0.0, rect.top() - step));
			else
				rect.moveTop(std::min(layout.height - rect.height(), rect.top() + step));
			update_geometry(source, index, rect, layout);
		}
		obs_source_release(source);
		canvas->update();
		refresh_controls();
	}

	void reset_selected_content()
	{
		if (!unlock->isChecked())
			return;
		obs_source_t *source = selected_source();
		if (!source)
			return;
		push_undo();
		update_content(source, canvas->selection(), 1.0, 0.0, 0.0);
		obs_source_release(source);
		canvas->update();
		refresh_controls();
	}

	QLabel *program_label = nullptr;
	QComboBox *layout_selector = nullptr;
	QCheckBox *unlock = nullptr;
	QLabel *lock_status = nullptr;
	LayoutCanvas *canvas = nullptr;
	QLabel *selected_label = nullptr;
	QComboBox *step_selector = nullptr;
	std::array<QPushButton *, 4> nudge_buttons{};
	QSlider *zoom = nullptr;
	QSlider *pan_x = nullptr;
	QSlider *pan_y = nullptr;
	QPushButton *undo = nullptr;
	QPushButton *reset_content = nullptr;
	QTimer *timer = nullptr;
	QString program_uuid;
	QString layout_signature;
	std::vector<UndoEntry> undo_stack;
};

LiveControl *live_control = nullptr;

} // namespace

extern "C" bool live_control_dock_init(void)
{
	if (live_control)
		return true;
	live_control = new LiveControl;
	if (!obs_frontend_add_dock_by_id(DOCK_ID, obs_module_text("Live.DockTitle"), live_control)) {
		delete live_control;
		live_control = nullptr;
		return false;
	}
	QTimer::singleShot(0, live_control, []() {
		if (live_control && live_control->parentWidget())
			live_control->parentWidget()->show();
	});
	blog(LOG_INFO, "[obs-box-layouts] live control dock registered");
	return true;
}

extern "C" void live_control_dock_shutdown(void)
{
	if (!live_control)
		return;
	obs_frontend_remove_dock(DOCK_ID);
	live_control = nullptr;
}
