#include "ut.h"

#include "inspector_widget.h"
#include "vmt/value.h"

#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>

#include <cmath>
#include <string>

namespace {

constexpr const char* kSchema = R"json({
  "format": "vmt-schema/1",
  "shader": "InspectorProbe",
  "groups": [{"name": "Base", "parameters": [
    {"name": "$alpha", "type": "float", "default": "0"},
    {"name": "$basetexture", "type": "texture", "description": "Diffuse texture"},
    {"name": "$enabled", "type": "bool", "default": "0"},
    {"name": "$mode", "type": "enum", "values": ["alpha", "beta"], "default": "alpha"},
    {"name": "$scale", "type": "float", "min": 0, "max": 1, "default": "0.25"},
    {"name": "$detail", "type": "string", "advanced": true, "default": "detail"}
  ]}]
})json";

constexpr const char* kDocument = R"vmt("InspectorProbe"
{
    "$alpha" "0"
    "$basetexture" "brick/original"
    "$enabled" "0"
    "$mode" "alpha"
    "$scale" "0.25"
}
)vmt";

void show_inspector(ui::qt::InspectorWidget& inspector) {
    inspector.resize(600, 180);
    inspector.show();
    inspector.activateWindow();
    QApplication::processEvents();
}

bool near(double actual, double expected) {
    return std::abs(actual - expected) < 0.000001;
}

} // namespace

UT_TEST_CASE("Qt inspector: value refresh preserves editors, focus and local partial text without feedback") {
    auto schemas = vmt::SchemaLibrary::load_json(kSchema, "inspector.json");
    auto parsed = vmt::Document::parse(kDocument);
    UT_CHECK(schemas.ok());
    UT_CHECK(parsed.ok());
    if (!schemas || !parsed) return;
    auto& document = parsed.value();
    int changes = 0;
    int shader_changes = 0;
    int advanced_changes = 0;
    ui::qt::InspectorWidget inspector;
    QObject::connect(&inspector, &ui::qt::InspectorWidget::parameterChanged,
        [&](const QString&, const QString&) { ++changes; });
    QObject::connect(&inspector, &ui::qt::InspectorWidget::shaderChanged,
        [&](const QString&) { ++shader_changes; });
    QObject::connect(&inspector, &ui::qt::InspectorWidget::showAdvancedToggled,
        [&](bool) { ++advanced_changes; });
    inspector.updateFromDocument(&document, schemas.value(), false);
    show_inspector(inspector);

    QPointer<QLineEdit> alpha = inspector.findChild<QLineEdit*>(QStringLiteral("$alpha"));
    QPointer<QLineEdit> texture = inspector.findChild<QLineEdit*>(QStringLiteral("$basetexture"));
    auto* check = inspector.findChild<QCheckBox*>(QStringLiteral("$enabled"));
    auto* combo = inspector.findChild<QComboBox*>(QStringLiteral("$mode"));
    auto* spin = inspector.findChild<QDoubleSpinBox*>(QStringLiteral("$scale"));
    auto* slider = inspector.findChild<QSlider*>(QStringLiteral("$scale:slider"));
    auto* scroll = inspector.findChild<QScrollArea*>();
    UT_CHECK(alpha && texture && check && combo && spin && slider && scroll);
    if (!alpha || !texture || !check || !combo || !spin || !slider || !scroll) return;
    alpha->setFocus();
    alpha->setCursorPosition(1);
    UT_CHECK(scroll->verticalScrollBar()->maximum() > 0);
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    const int scroll_position = scroll->verticalScrollBar()->value();

    UT_CHECK(document.set_parameter("$basetexture", "brick/changed").ok());
    UT_CHECK(document.set_parameter("$enabled", "1").ok());
    UT_CHECK(document.set_parameter("$mode", "beta").ok());
    UT_CHECK(document.set_parameter("$scale", "0.75").ok());
    inspector.updateFromDocument(&document, schemas.value(), false);
    UT_CHECK(alpha && texture);
    if (!alpha || !texture) return;
    UT_CHECK(inspector.findChild<QLineEdit*>(QStringLiteral("$alpha")) == alpha.data());
    UT_CHECK(inspector.findChild<QLineEdit*>(QStringLiteral("$basetexture")) == texture.data());
    UT_CHECK(alpha->hasFocus());
    UT_CHECK(alpha->cursorPosition() == 1);
    UT_CHECK(texture->text() == QStringLiteral("brick/changed"));
    UT_CHECK(check->isChecked());
    UT_CHECK(combo->currentText() == QStringLiteral("beta"));
    UT_CHECK(near(spin->value(), 0.75));
    UT_CHECK(slider->value() == 75);
    UT_CHECK(scroll->verticalScrollBar()->value() == scroll_position);
    UT_CHECK(changes == 0 && shader_changes == 0 && advanced_changes == 0);

    // Raw text can contain a value outside the schema's choices. Display it
    // without retaining a stale selection or adding invalid options.
    UT_CHECK(document.set_parameter("$mode", "unlisted").ok());
    inspector.updateFromDocument(&document, schemas.value(), false);
    UT_CHECK(combo->currentIndex() == -1);
    UT_CHECK(combo->placeholderText() == QStringLiteral("unlisted"));
    UT_CHECK(combo->count() == 2);
    UT_CHECK(document.set_parameter("$mode", "alpha").ok());
    inspector.updateFromDocument(&document, schemas.value(), false);
    UT_CHECK(combo->currentText() == QStringLiteral("alpha"));

    // A schema row already exists while its parameter is absent: changing
    // presence must update the value without replacing the control.
    QPointer<QCheckBox> enabled = check;
    UT_CHECK(document.remove_parameter("$enabled").ok());
    inspector.updateFromDocument(&document, schemas.value(), false);
    UT_CHECK(enabled && !enabled->isChecked());
    UT_CHECK(document.set_bool("$enabled", true).ok());
    inspector.updateFromDocument(&document, schemas.value(), false);
    UT_CHECK(enabled && enabled->isChecked());
    UT_CHECK(changes == 0 && shader_changes == 0 && advanced_changes == 0);

    // A synchronous edit callback can leave the document's canonical value
    // unchanged: typing "0." must retain the period and cursor in the editor.
    QObject::connect(&inspector, &ui::qt::InspectorWidget::parameterChanged,
        [&](const QString& name, const QString& value) {
            if (name != QStringLiteral("$alpha")) return;
            const auto number = vmt::parse_float(value.toStdString());
            UT_CHECK(number.ok());
            if (number) UT_CHECK(document.set_float("$alpha", number.value()).ok());
            inspector.updateFromDocument(&document, schemas.value(), false);
        });
    QKeyEvent period(QEvent::KeyPress, Qt::Key_Period, Qt::NoModifier, QStringLiteral("."));
    QApplication::sendEvent(alpha.data(), &period);
    UT_CHECK(alpha && alpha->text() == QStringLiteral("0."));
    UT_CHECK(alpha && alpha->cursorPosition() == 2 && alpha->hasFocus());
    UT_CHECK(changes == 1);

    // An actual external edit (including undo) must still replace a focused
    // editor's value without emitting another parameter edit.
    UT_CHECK(document.set_parameter("$alpha", "0.5").ok());
    inspector.updateFromDocument(&document, schemas.value(), false);
    UT_CHECK(alpha && alpha->text() == QStringLiteral("0.5"));
    UT_CHECK(changes == 1);
}

UT_TEST_CASE("Qt inspector: slider and spin updates synchronize once without rebuilding the active control") {
    auto schemas = vmt::SchemaLibrary::load_json(kSchema, "inspector.json");
    auto parsed = vmt::Document::parse(kDocument);
    UT_CHECK(schemas.ok() && parsed.ok());
    if (!schemas || !parsed) return;
    int changes = 0;
    ui::qt::InspectorWidget inspector;
    inspector.updateFromDocument(&parsed.value(), schemas.value(), false);
    auto* spin = inspector.findChild<QDoubleSpinBox*>(QStringLiteral("$scale"));
    QPointer<QSlider> slider = inspector.findChild<QSlider*>(QStringLiteral("$scale:slider"));
    UT_CHECK(spin && slider);
    if (!spin || !slider) return;
    QObject::connect(&inspector, &ui::qt::InspectorWidget::parameterChanged,
        [&](const QString& name, const QString& value) {
            ++changes;
            UT_CHECK(name == QStringLiteral("$scale"));
            UT_CHECK(parsed.value().set_parameter(name.toStdString(), value.toStdString()).ok());
            inspector.updateFromDocument(&parsed.value(), schemas.value(), false);
        });

    slider->setValue(80);
    UT_CHECK(changes == 1);
    UT_CHECK(slider && inspector.findChild<QSlider*>(QStringLiteral("$scale:slider")) == slider.data());
    if (!slider) return;
    UT_CHECK(near(spin->value(), 0.8));
    spin->setValue(0.35);
    UT_CHECK(changes == 2);
    UT_CHECK(slider && slider->value() == 35);
    UT_CHECK(inspector.findChild<QDoubleSpinBox*>(QStringLiteral("$scale")) == spin);
}

UT_TEST_CASE("Qt inspector: filters retain controls while unknown rows, advanced fields and schemas refresh structure") {
    auto schemas = vmt::SchemaLibrary::load_json(kSchema, "inspector.json");
    auto parsed = vmt::Document::parse(kDocument);
    UT_CHECK(schemas.ok() && parsed.ok());
    if (!schemas || !parsed) return;
    auto& document = parsed.value();
    ui::qt::InspectorWidget inspector;
    inspector.updateFromDocument(&document, schemas.value(), false);
    show_inspector(inspector);
    QPointer<QLineEdit> texture = inspector.findChild<QLineEdit*>(QStringLiteral("$basetexture"));
    auto* filter = inspector.findChild<QLineEdit*>(QStringLiteral("parameterFilter"));
    UT_CHECK(texture && filter);
    if (!texture || !filter) return;
    texture->setText(QStringLiteral("local draft"));
    filter->setText(QStringLiteral("scale"));
    UT_CHECK(texture && !texture->isVisibleTo(&inspector));
    filter->setText(QStringLiteral("diffuse"));
    UT_CHECK(texture && texture->isVisibleTo(&inspector));
    UT_CHECK(texture && texture->text() == QStringLiteral("local draft"));
    UT_CHECK(inspector.findChild<QLineEdit*>(QStringLiteral("$basetexture")) == texture.data());
    filter->clear();

    filter->setText(QStringLiteral("scale"));
    UT_CHECK(document.set_parameter("$basetexture", "brick/hidden-update").ok());
    inspector.updateFromDocument(&document, schemas.value(), false);
    filter->clear();
    UT_CHECK(texture && texture->text() == QStringLiteral("brick/hidden-update"));

    UT_CHECK(document.set_parameter("$mode", "unlisted").ok());
    UT_CHECK(document.set_parameter("$custom", "first").ok());
    inspector.updateFromDocument(&document, schemas.value(), false);
    auto* custom = inspector.findChild<QLineEdit*>(QStringLiteral("$custom"));
    UT_CHECK(custom && custom->text() == QStringLiteral("first"));
    auto* raw_mode = inspector.findChild<QComboBox*>(QStringLiteral("$mode"));
    UT_CHECK(raw_mode && raw_mode->currentIndex() == -1);
    UT_CHECK(raw_mode && raw_mode->placeholderText() == QStringLiteral("unlisted"));
    UT_CHECK(document.remove_parameter("$custom").ok());
    inspector.updateFromDocument(&document, schemas.value(), false);
    UT_CHECK(inspector.findChild<QLineEdit*>(QStringLiteral("$custom")) == nullptr);

    inspector.updateFromDocument(&document, schemas.value(), true);
    auto* detail = inspector.findChild<QLineEdit*>(QStringLiteral("$detail"));
    UT_CHECK(detail && detail->isVisibleTo(&inspector));
    UT_CHECK(detail && detail->text() == QStringLiteral("detail"));
    inspector.updateFromDocument(&document, schemas.value(), false);
    detail = inspector.findChild<QLineEdit*>(QStringLiteral("$detail"));
    UT_CHECK(detail == nullptr || !detail->isVisibleTo(&inspector));

    std::string changed_schema(kSchema);
    const std::string range = "\"max\": 1";
    changed_schema.replace(changed_schema.find(range), range.size(), "\"max\": 10");
    auto wider = vmt::SchemaLibrary::load_json(changed_schema, "wider.json");
    UT_CHECK(wider.ok());
    if (!wider) return;
    inspector.updateFromDocument(&document, wider.value(), false);
    auto* spin = inspector.findChild<QDoubleSpinBox*>(QStringLiteral("$scale"));
    UT_CHECK(spin && near(spin->maximum(), 10));

    const char* alternate = R"json({"format":"vmt-schema/1","shader":"AlternateProbe",
      "groups":[{"name":"Other","parameters":[{"name":"$other","type":"string"}]}]})json";
    UT_CHECK(schemas.value().add_json(alternate, "alternate.json").ok());
    auto other_document = vmt::Document::parse("AlternateProbe { \"$other\" \"new shader\" }");
    UT_CHECK(other_document.ok());
    if (!other_document) return;
    inspector.updateFromDocument(&other_document.value(), schemas.value(), false);
    auto* other = inspector.findChild<QLineEdit*>(QStringLiteral("$other"));
    UT_CHECK(other && other->text() == QStringLiteral("new shader"));
    UT_CHECK(inspector.findChild<QLineEdit*>(QStringLiteral("$basetexture")) == nullptr);
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    return ut::run_all();
}
