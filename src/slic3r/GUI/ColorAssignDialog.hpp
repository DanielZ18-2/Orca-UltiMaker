#ifndef slic3r_ColorAssignDialog_hpp_
#define slic3r_ColorAssignDialog_hpp_

#include "GUI_Utils.hpp"

#include <wx/colourdata.h>
#include <wx/statbmp.h>

#include <string>
#include <vector>

class ComboBox;

namespace Slic3r { class ModelObject; }

namespace Slic3r { namespace GUI {

// MakerBot / UltiMaker Fork: assign the colours a 3MF brings to the filament
// slots the selected printer actually has.
//
// Why this dialog exists at all: a file from a sharing platform is made for
// the machine of whoever uploaded it. On a machine with fewer filaments than
// the file uses, something has to give, and until now the import decided on
// its own -- first by silently adding slots the printer does not have, then
// (Paket 172) by merging colours without asking.
//
// Merging loses no face; the leaf count is checked inside
// ModelVolume::remap_extruders() on every call. It loses the DISTINCTION:
// afterwards the plate no longer knows which face carried which colour.
// Which of the two matters is the user's call.
//
// Two things this dialog does that the automatic step could not:
//
//  - It compares against THE PRINTER's colours. The automatic mapping
//    compared the surplus colour against the colours of the FILE, which is
//    the wrong set: in geometry-only import the user keeps their own
//    filaments. Measured on a Method X carrying Generic ABS-R and
//    Generic SR-30, the automatic step produced "5 -> 1" on that basis.
//
//  - It shows the MATERIAL of each slot. In the same test a model colour
//    landed on Generic SR-30, water soluble support material, and nothing on
//    screen said so.
//
// The target is chosen in a combo box, not only by dragging. Orca's
// DragDropPanel is mouse-only; this has to work from the keyboard.
//
// The upper half is editable, the lower half is not, and that is deliberate:
// the printer's slot colour is a property of the machine in front of the
// user, the model's colour is a fact carried in the file. A slot's colour is
// changed by clicking its swatch or by its "Change" button -- both open
// Orca's own "Please choose the filament colour" picker
// (show_sys_picker_dialog(), wxExtensions.hpp), the same dialog the sidebar
// opens, not a second one of our own. The button exists because a click
// cannot be done from the keyboard.
class ColorAssignDialog : public DPIDialog
{
public:
    // One filament slot of the selected printer.
    struct Slot
    {
        std::string colour;   // "#RRGGBB", as project_config carries it
        std::string preset;   // the filament preset's name
        std::string material; // filament_type -- support material must be visible
    };

    // model_colours  the file's filament colours, index 0 is filament 1
    // slots          the printer's filament slots, index 0 is slot 1
    // proposal       proposal[i] = 1 based slot for model colour i+1;
    //                may be shorter than model_colours, the rest defaults to
    //                its own index clamped into the slot range
    // objects        the model that is about to be imported, for the preview.
    //                It is READ, never changed: the preview is rendered from a
    //                colour list handed to the renderer, so nothing is merged
    //                before the user closes the dialog with OK.
    ColorAssignDialog(wxWindow                                *parent,
                      const wxString                          &filename,
                      const std::vector<std::string>          &model_colours,
                      const std::vector<Slot>                 &slots,
                      const std::vector<int>                  &proposal,
                      const std::vector<Slic3r::ModelObject *> &objects);

    // mapping()[i] is the 1 based slot that filament i goes to, with index 0
    // mapping to itself so it can be handed to ModelObject::remap_extruders()
    // unchanged.
    std::vector<int> mapping() const;

    // true when the user asked for the model's colours to be written into the
    // printer's filament slots.
    bool adopt_colours() const { return m_adopt_colours; }

    // What the printer's filament slots should carry after OK, one entry per
    // slot, ready to be written into filament_colour. Three sources, in this
    // order of precedence:
    //
    //   1  a colour the user picked by hand in this dialog
    //   2  the model's colour, where "adopt" is ticked and a model colour
    //      points at that slot -- the first one wins, because two colours
    //      merged onto one slot cannot both have it
    //   3  what the slot carried when the dialog opened
    //
    // Hand beats file: a user who opens the picker for slot 2 has said
    // something more specific than a tick box can say.
    std::vector<std::string> final_slot_colours() const;

    // true when at least one slot colour was picked by hand.
    bool slot_colours_edited() const;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    // Opens Orca's filament colour picker for one slot and keeps the answer.
    void pick_slot_colour(size_t index);

    // Draws the model as it would come out with the assignment as it stands.
    //
    // Nothing in the model is touched. GLCanvas3D::render_thumbnail() takes the
    // colour list as an argument, so the picture can show a merge that has not
    // happened yet -- which is exactly what "merge only when the dialog closes"
    // needs.
    void refresh_preview();

    // Puts the colour of the chosen slot into the middle patch of every model
    // row, so that a row reads "this colour becomes that one". Redrawn by
    // refresh_preview() together with the picture: the row and the picture
    // answer the same question and must never disagree.
    void paint_slot_echo();

    // Puts the colours onto the swatches.
    //
    // This runs AFTER GUI_App::UpdateDlgDarkUI(), and it has to: that pass
    // walks every child and pushes its background through
    // StateColor::darkModeColorFor(), whose table maps #FFFFFF to #2D2D31 and
    // #000000 to #FFFFFE. For a theme colour that is right. For a filament
    // colour it is a lie -- white filament would be drawn near black. So the
    // swatches are painted last, and nothing repaints them afterwards.
    void paint_swatches();

    std::vector<ComboBox *>  m_targets;        // one per model colour
    std::vector<std::string> m_model_colours;
    std::vector<Slic3r::ModelObject *> m_objects; // read only, for the preview
    wxStaticBitmap          *m_preview{nullptr};
    std::vector<wxWindow *>  m_model_patches;  // one per model colour
    std::vector<wxWindow *>  m_slot_echo;      // one per model colour
    std::vector<wxWindow *>  m_slot_patches;   // one per printer slot
    std::vector<std::string> m_slot_colours;   // as edited in this dialog
    std::vector<bool>        m_slot_edited;    // true where the user picked
    wxColourData             m_clrData;        // keeps the custom colours alive
    size_t                   m_slot_count{0};
    bool                     m_adopt_colours{false};
};

}} // namespace Slic3r::GUI

#endif /* slic3r_ColorAssignDialog_hpp_ */
