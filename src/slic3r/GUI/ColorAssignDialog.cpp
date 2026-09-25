#include "ColorAssignDialog.hpp"

#include "3DScene.hpp"
#include "Camera.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"

#include <boost/log/trivial.hpp>

#include <wx/cursor.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

#include <algorithm>

namespace Slic3r { namespace GUI {

// A "#RRGGBB" string as wxWidgets wants it. An unreadable value becomes grey
// rather than black, so that "no colour given" does not look like "black".
static wxColour colour_from_hex(const std::string &hex)
{
    wxColour out;
    if (! hex.empty() && out.Set(wxString::FromUTF8(hex.c_str())))
        return out;
    return wxColour(0x9A, 0x9A, 0x9A);
}

// A plain filled rectangle. Deliberately not a ColorPanel from
// DragDropPanel.hpp: that one carries drag and drop behaviour this dialog
// does not want, because dragging cannot be done from the keyboard.
//
// The colour itself is put on in paint_swatches(), after the dark mode pass.
static wxPanel *colour_patch(wxWindow *parent, const wxSize &size)
{
    auto *patch = new wxPanel(parent, wxID_ANY, wxDefaultPosition, size, wxBORDER_SIMPLE);
    patch->SetMinSize(size);
    return patch;
}

ColorAssignDialog::ColorAssignDialog(wxWindow                                *parent,
                                     const wxString                          &filename,
                                     const std::vector<std::string>          &model_colours,
                                     const std::vector<Slot>                 &slots,
                                     const std::vector<int>                  &proposal,
                                     const std::vector<Slic3r::ModelObject *> &objects)
    : DPIDialog(parent, wxID_ANY, _L("Assign the model's colours"), wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_model_colours(model_colours)
    , m_objects(objects)
    , m_slot_count(slots.size())
{
    const wxSize patch_size = wxSize(FromDIP(26), FromDIP(16));
    // A swatch that answers to a click is a control, and a control that small
    // is hard to hit. The printer's swatches are therefore bigger than the
    // model's, which only have to be looked at.
    const wxSize swatch_size = wxSize(FromDIP(40), FromDIP(22));

    m_slot_colours.reserve(slots.size());
    for (const Slot &slot : slots)
        m_slot_colours.push_back(slot.colour);
    m_slot_edited.assign(slots.size(), false);

    auto *main = new wxBoxSizer(wxVERTICAL);

    auto *head = new wxStaticText(
        this, wxID_ANY,
        wxString::Format(_L("%s uses %d colours. Your printer has %d filament slots."), filename,
                         int(model_colours.size()), int(slots.size())));
    head->SetFont(::Label::Body_14);
    head->SetMaxSize(wxSize(FromDIP(460), -1));
    head->Wrap(FromDIP(460));
    main->Add(head, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    main->AddSpacer(FromDIP(10));

    // --- the printer's slots: this is what the model's colours are compared
    // --- against, and the material has to be readable before anyone picks it
    auto *slot_box   = new wxStaticBoxSizer(wxVERTICAL, this, _L("Slots of your printer"));
    auto *slot_grid  = new wxFlexGridSizer(0, 4, FromDIP(6), FromDIP(10));
    slot_grid->AddGrowableCol(2, 1);
    for (size_t i = 0; i < slots.size(); ++i) {
        auto *swatch = colour_patch(slot_box->GetStaticBox(), swatch_size);
        swatch->SetToolTip(_L("Click to choose the colour of this filament slot"));
        swatch->SetCursor(wxCursor(wxCURSOR_HAND));
        swatch->Bind(wxEVT_LEFT_UP, [this, i](wxMouseEvent &) { pick_slot_colour(i); });
        m_slot_patches.push_back(swatch);
        slot_grid->Add(swatch, 0, wxALIGN_CENTER_VERTICAL);

        slot_grid->Add(new wxStaticText(slot_box->GetStaticBox(), wxID_ANY,
                                        wxString::Format("%d", int(i + 1))),
                       0, wxALIGN_CENTER_VERTICAL);

        wxString text = wxString::FromUTF8(slots[i].preset.c_str());
        if (! slots[i].material.empty())
            text += "   " + wxString::FromUTF8(slots[i].material.c_str());
        slot_grid->Add(new wxStaticText(slot_box->GetStaticBox(), wxID_ANY, text), 0,
                       wxALIGN_CENTER_VERTICAL);

        // A click cannot be done from the keyboard, so the same thing gets a
        // button that Tab reaches and Space presses. It carries a word, not
        // an icon: a name a screen reader can read out.
        auto *change = new ::Button(slot_box->GetStaticBox(), _L("Change"));
        change->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        change->SetToolTip(wxString::Format(_L("Choose the colour of slot %d"), int(i + 1)));
        change->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent &) { pick_slot_colour(i); });
        slot_grid->Add(change, 0, wxALIGN_CENTER_VERTICAL);
    }
    slot_box->Add(slot_grid, 0, wxEXPAND | wxALL, FromDIP(8));

    // The second half now shows two colours per row, so the old one line
    // ("the colours below come from the file") would be ambiguous: one of the
    // two does change.
    auto *slot_hint = new wxStaticText(
        slot_box->GetStaticBox(), wxID_ANY,
        _L("Below, the left colour is the model's and comes from the file. The one beside it is "
           "the filament it would be printed in."));
    slot_hint->SetMaxSize(wxSize(FromDIP(440), -1));
    slot_hint->Wrap(FromDIP(440));
    slot_box->Add(slot_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    main->Add(slot_box, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    main->AddSpacer(FromDIP(10));

    // --- the model's colours, each with the slot it goes to
    auto *model_box  = new wxStaticBoxSizer(wxVERTICAL, this, _L("Colours of the model"));
    // Four columns: the model's colour, its number, the colour of the slot it
    // points at, and the slot itself. The third one is the answer to the
    // question the second half asks -- "which filament colour does this region
    // become" -- written next to the question instead of only in the picture.
    auto *model_grid = new wxFlexGridSizer(0, 4, FromDIP(4), FromDIP(10));
    model_grid->AddGrowableCol(3, 1);

    wxArrayString slot_labels;
    for (size_t s = 0; s < slots.size(); ++s) {
        wxString label = wxString::Format(_L("Slot %d"), int(s + 1));
        if (! slots[s].material.empty())
            label += "   " + wxString::FromUTF8(slots[s].material.c_str());
        slot_labels.Add(label);
    }

    for (size_t i = 0; i < model_colours.size(); ++i) {
        auto *patch = colour_patch(model_box->GetStaticBox(), patch_size);
        m_model_patches.push_back(patch);
        model_grid->Add(patch, 0, wxALIGN_CENTER_VERTICAL);

        model_grid->Add(new wxStaticText(model_box->GetStaticBox(), wxID_ANY,
                                         wxString::Format("%d", int(i + 1))),
                        0, wxALIGN_CENTER_VERTICAL);

        // What this region turns into. Filled in by paint_slot_echo(), which
        // runs on every change, so the two colours in a row always stand for
        // "from" and "to".
        auto *echo = colour_patch(model_box->GetStaticBox(), patch_size);
        echo->SetToolTip(_L("The filament colour this region will be printed in"));
        m_slot_echo.push_back(echo);
        model_grid->Add(echo, 0, wxALIGN_CENTER_VERTICAL);

        auto *target = new ComboBox(model_box->GetStaticBox(), wxID_ANY, wxEmptyString,
                                    wxDefaultPosition, wxSize(FromDIP(240), -1), 0, nullptr,
                                    wxCB_READONLY);
        for (const wxString &label : slot_labels)
            target->Append(label);

        // The proposal is a default, never a decision. Where it says nothing,
        // filament i keeps its own number, clamped into the slots that exist.
        int chosen = int(i) + 1;
        if (i < proposal.size() && proposal[i] >= 1)
            chosen = proposal[i];
        chosen = std::min(chosen, int(slots.size()));
        chosen = std::max(chosen, 1);
        target->SetSelection(chosen - 1);

        // Every change redraws the picture. SetSelection() above does not send
        // this event -- only a choice made by hand does -- so filling the boxes
        // cannot set off a round of renders.
        target->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
            refresh_preview();
            e.Skip();
        });

        model_grid->Add(target, 0, wxALIGN_CENTER_VERTICAL);
        m_targets.push_back(target);
    }
    model_box->Add(model_grid, 0, wxEXPAND | wxALL, FromDIP(8));

    // --- the picture, beside the colours rather than below them: the dialog
    // --- gets wider instead of taller, and the two things that have to be
    // --- compared stand next to each other
    auto *preview_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("How it would come out"));
    m_preview = new wxStaticBitmap(preview_box->GetStaticBox(), wxID_ANY, wxNullBitmap,
                                   wxDefaultPosition, wxSize(FromDIP(340), FromDIP(340)));
    m_preview->SetMinSize(wxSize(FromDIP(340), FromDIP(340)));
    preview_box->Add(m_preview, 0, wxALL, FromDIP(8));

    auto *middle = new wxBoxSizer(wxHORIZONTAL);
    middle->Add(model_box, 1, wxEXPAND);
    middle->AddSpacer(FromDIP(10));
    middle->Add(preview_box, 0, wxALIGN_TOP);
    main->Add(middle, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    main->AddSpacer(FromDIP(10));

    auto *adopt = new ::CheckBox(this);
    adopt->SetValue(false);
    auto *adopt_row = new wxBoxSizer(wxHORIZONTAL);
    adopt_row->Add(adopt, 0, wxALIGN_CENTER_VERTICAL);
    adopt_row->AddSpacer(FromDIP(8));
    auto *adopt_text = new wxStaticText(
        this, wxID_ANY, _L("Also take the model's colours over into the printer's filament slots"));
    adopt_text->SetMaxSize(wxSize(FromDIP(420), -1));
    adopt_text->Wrap(FromDIP(420));
    adopt_row->Add(adopt_text, 1, wxALIGN_CENTER_VERTICAL);
    main->Add(adopt_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    const wxString adopt_tip = _L("A slot whose colour you changed above keeps your colour.");
    adopt->SetToolTip(adopt_tip);
    adopt_text->SetToolTip(adopt_tip);

    adopt->Bind(wxEVT_TOGGLEBUTTON, [this, adopt](wxCommandEvent &e) {
        m_adopt_colours = adopt->GetValue();
        refresh_preview();
        e.Skip();
    });

    main->AddSpacer(FromDIP(10));

    auto *buttons = new DialogButtons(this, {"OK", "Cancel"});
    buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_OK); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    main->Add(buttons, 0, wxEXPAND);

    SetSizer(main);
    Layout();
    Fit();
    Centre(wxBOTH);
    wxGetApp().UpdateDlgDarkUI(this);

    // Last, so that the dark mode pass cannot recolour a filament. See the
    // note on paint_swatches() in the header.
    paint_swatches();
    refresh_preview();
}

void ColorAssignDialog::paint_slot_echo()
{
    const std::vector<int>         map  = mapping();
    const std::vector<std::string> cols = final_slot_colours();

    for (size_t i = 0; i < m_slot_echo.size(); ++i) {
        std::string hex;
        if (i + 1 < map.size() && map[i + 1] >= 1) {
            const size_t slot = size_t(map[i + 1] - 1);
            if (slot < cols.size())
                hex = cols[slot];
        }
        m_slot_echo[i]->SetBackgroundColour(colour_from_hex(hex));
        m_slot_echo[i]->Refresh();
    }
}

void ColorAssignDialog::refresh_preview()
{
    // The row and the picture answer the same question and must never
    // disagree, so they are redrawn together, never separately.
    paint_slot_echo();

    if (m_preview == nullptr || m_objects.empty())
        return;

    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    GLCanvas3D *canvas = plater->get_view3D_canvas3D();
    if (canvas == nullptr)
        return;

    // The colour each of the model's filaments would be printed in: not its own
    // colour, but the colour of the slot it points at. Two colours on one slot
    // therefore look the same here -- which is the whole question the user is
    // being asked, shown instead of described.
    const std::vector<int>         map  = mapping();
    const std::vector<std::string> cols = final_slot_colours(); // honours the tick box
    std::vector<ColorRGBA>         colours;
    colours.reserve(m_model_colours.size());
    for (size_t i = 0; i < m_model_colours.size(); ++i) {
        std::string hex = m_model_colours[i];
        if (i + 1 < map.size() && map[i + 1] >= 1) {
            const size_t slot = size_t(map[i + 1] - 1);
            if (slot < cols.size() && ! cols[slot].empty())
                hex = cols[slot];
        }
        const wxColour c = colour_from_hex(hex);
        colours.push_back(ColorRGBA(c.Red() / 255.0f, c.Green() / 255.0f, c.Blue() / 255.0f, 1.0f));
    }
    if (colours.empty())
        return;

    // Every volume of every object, not just the first one of the first object:
    // Plater::update_obj_preview_thumbnail() renders a single volume, which is
    // enough for the OBJ colour dialog and not enough for a plate that carries
    // seven objects.
    ModelObjectPtrs    objects = m_objects;
    GLVolumeCollection volumes;
    for (size_t o = 0; o < objects.size(); ++o) {
        if (objects[o] == nullptr)
            continue;
        for (size_t v = 0; v < objects[o]->volumes.size(); ++v)
            volumes.load_object_volume(objects[o], int(o), int(v), 0, "volume", true, false, false, false);
    }
    if (volumes.volumes.empty())
        return;

    ThumbnailData    data;
    ThumbnailsParams params = {{}, false, true, false, true, 0, false};
    canvas->render_thumbnail(data, colours, PartPlate::plate_thumbnail_width,
                             PartPlate::plate_thumbnail_height, params, objects, volumes,
                             Camera::EType::Ortho, Camera::ViewAngleType::Iso, false, false);
    if (! data.is_valid()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": no preview -- the renderer returned nothing";
        return;
    }

    wxImage image(data.width, data.height);
    image.InitAlpha();
    for (unsigned int r = 0; r < data.height; ++r) {
        const unsigned int rr = (data.height - 1 - r) * data.width;
        for (unsigned int c = 0; c < data.width; ++c) {
            const unsigned char *px = (const unsigned char *) data.pixels.data() + 4 * (rr + c);
            image.SetRGB(int(c), int(r), px[0], px[1], px[2]);
            image.SetAlpha(int(c), int(r), px[3]);
        }
    }
    const int side = FromDIP(340);
    image = image.Rescale(side, side, wxIMAGE_QUALITY_HIGH);
    m_preview->SetBitmap(wxBitmap(image));
    m_preview->Refresh();
}

void ColorAssignDialog::paint_swatches()
{
    for (size_t i = 0; i < m_slot_patches.size(); ++i) {
        if (i >= m_slot_colours.size())
            break;
        m_slot_patches[i]->SetBackgroundColour(colour_from_hex(m_slot_colours[i]));
        m_slot_patches[i]->Refresh();
    }
    for (size_t i = 0; i < m_model_patches.size(); ++i) {
        if (i >= m_model_colours.size())
            break;
        m_model_patches[i]->SetBackgroundColour(colour_from_hex(m_model_colours[i]));
        m_model_patches[i]->Refresh();
    }
}

void ColorAssignDialog::pick_slot_colour(size_t index)
{
    if (index >= m_slot_colours.size())
        return;

    wxColour current = colour_from_hex(m_slot_colours[index]);
    if (! current.IsOk())
        current = wxColour(0, 0, 0);
    m_clrData.SetColour(current);

    // Orca's own picker, the one the sidebar opens: "Please choose the
    // filament colour". It returns its argument unchanged when the user
    // cancels, so the answer has to be compared against what went in --
    // exactly what PlaterPresetComboBox::show_default_color_picker() does.
    const wxColourData data = ::show_sys_picker_dialog(this, m_clrData);
    if (data.GetColour() == m_clrData.GetColour())
        return;

    m_clrData.SetColour(data.GetColour());
    m_slot_colours[index] = data.GetColour().GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    m_slot_edited[index]  = true;

    if (index < m_slot_patches.size()) {
        m_slot_patches[index]->SetBackgroundColour(data.GetColour());
        m_slot_patches[index]->Refresh();
    }
    refresh_preview();
}

std::vector<int> ColorAssignDialog::mapping() const
{
    // Index 0 stands for "take the extruder of the volume" in a painting and
    // must map to itself, so the vector is one longer than the colour list.
    std::vector<int> out(m_model_colours.size() + 1, 0);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = int(i);

    for (size_t i = 0; i < m_targets.size(); ++i) {
        const int chosen = m_targets[i]->GetSelection();
        if (chosen >= 0 && size_t(chosen) < m_slot_count)
            out[i + 1] = chosen + 1;
    }
    return out;
}

bool ColorAssignDialog::slot_colours_edited() const
{
    for (bool edited : m_slot_edited)
        if (edited)
            return true;
    return false;
}

std::vector<std::string> ColorAssignDialog::final_slot_colours() const
{
    std::vector<std::string> out = m_slot_colours;
    if (! m_adopt_colours)
        return out;

    const std::vector<int> map = mapping();
    for (size_t s = 0; s < out.size(); ++s) {
        if (s < m_slot_edited.size() && m_slot_edited[s])
            continue; // hand beats file
        for (size_t i = 1; i < map.size(); ++i)
            if (map[i] == int(s + 1) && i - 1 < m_model_colours.size()) {
                out[s] = m_model_colours[i - 1];
                break; // two colours on one slot: the first one gets it
            }
    }
    return out;
}

void ColorAssignDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
