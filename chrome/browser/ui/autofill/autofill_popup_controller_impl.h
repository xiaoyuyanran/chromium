// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_AUTOFILL_AUTOFILL_POPUP_CONTROLLER_IMPL_H_
#define CHROME_BROWSER_UI_AUTOFILL_AUTOFILL_POPUP_CONTROLLER_IMPL_H_

#include "base/gtest_prod_util.h"
#include "base/i18n/rtl.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string16.h"
#include "chrome/browser/ui/autofill/autofill_popup_controller.h"
#include "chrome/browser/ui/autofill/popup_controller_common.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/rect.h"
#include "ui/gfx/rect_f.h"

namespace autofill {

class AutofillPopupDelegate;
class AutofillPopupView;

// This class is a controller for an AutofillPopupView. It implements
// AutofillPopupController to allow calls from AutofillPopupView. The
// other, public functions are available to its instantiator.
class AutofillPopupControllerImpl : public AutofillPopupController {
 public:
  // Creates a new |AutofillPopupControllerImpl|, or reuses |previous| if the
  // construction arguments are the same. |previous| may be invalidated by this
  // call. The controller will listen for keyboard input routed to
  // |web_contents| while the popup is showing, unless |web_contents| is NULL.
  static base::WeakPtr<AutofillPopupControllerImpl> GetOrCreate(
      base::WeakPtr<AutofillPopupControllerImpl> previous,
      base::WeakPtr<AutofillPopupDelegate> delegate,
      content::WebContents* web_contents,
      gfx::NativeView container_view,
      const gfx::RectF& element_bounds,
      base::i18n::TextDirection text_direction);

  // Shows the popup, or updates the existing popup with the given values.
  void Show(const std::vector<base::string16>& names,
            const std::vector<base::string16>& subtexts,
            const std::vector<base::string16>& icons,
            const std::vector<int>& identifiers);

  // Updates the data list values currently shown with the popup.
  void UpdateDataListValues(const std::vector<base::string16>& values,
                            const std::vector<base::string16>& labels);

  // Hides the popup and destroys the controller. This also invalidates
  // |delegate_|.
  virtual void Hide() OVERRIDE;

  // Invoked when the view was destroyed by by someone other than this class.
  virtual void ViewDestroyed() OVERRIDE;

  bool HandleKeyPressEvent(const content::NativeWebKeyboardEvent& event);

  // Tells the view to capture mouse events. Must be called before |Show()|.
  void set_hide_on_outside_click(bool hide_on_outside_click);

 protected:
  FRIEND_TEST_ALL_PREFIXES(AutofillExternalDelegateBrowserTest,
                           CloseWidgetAndNoLeaking);
  FRIEND_TEST_ALL_PREFIXES(AutofillPopupControllerUnitTest,
                           ProperlyResetController);

  AutofillPopupControllerImpl(base::WeakPtr<AutofillPopupDelegate> delegate,
                              content::WebContents* web_contents,
                              gfx::NativeView container_view,
                              const gfx::RectF& element_bounds,
                              base::i18n::TextDirection text_direction);
  virtual ~AutofillPopupControllerImpl();

  // AutofillPopupController implementation.
  virtual void UpdateBoundsAndRedrawPopup() OVERRIDE;
  virtual void SetSelectionAtPoint(const gfx::Point& point) OVERRIDE;
  virtual void AcceptSelectionAtPoint(const gfx::Point& point) OVERRIDE;
  virtual void SelectionCleared() OVERRIDE;
  virtual bool ShouldRepostEvent(const ui::MouseEvent& event) OVERRIDE;
  virtual bool ShouldHideOnOutsideClick() const OVERRIDE;
  virtual void AcceptSuggestion(size_t index) OVERRIDE;
  virtual int GetIconResourceID(
      const base::string16& resource_name) const OVERRIDE;
  virtual bool CanDelete(size_t index) const OVERRIDE;
  virtual bool IsWarning(size_t index) const OVERRIDE;
  virtual gfx::Rect GetRowBounds(size_t index) OVERRIDE;
  virtual void SetPopupBounds(const gfx::Rect& bounds) OVERRIDE;
  virtual const gfx::Rect& popup_bounds() const OVERRIDE;
  virtual gfx::NativeView container_view() OVERRIDE;
  virtual const gfx::RectF& element_bounds() const OVERRIDE;
  virtual bool IsRTL() const OVERRIDE;

  virtual const std::vector<base::string16>& names() const OVERRIDE;
  virtual const std::vector<base::string16>& subtexts() const OVERRIDE;
  virtual const std::vector<base::string16>& icons() const OVERRIDE;
  virtual const std::vector<int>& identifiers() const OVERRIDE;
#if !defined(OS_ANDROID)
  virtual const gfx::FontList& GetNameFontListForRow(
      size_t index) const OVERRIDE;
  virtual const gfx::FontList& subtext_font_list() const OVERRIDE;
#endif
  virtual int selected_line() const OVERRIDE;

  content::WebContents* web_contents();

  // Change which line is currently selected by the user.
  void SetSelectedLine(int selected_line);

  // Increase the selected line by 1, properly handling wrapping.
  void SelectNextLine();

  // Decrease the selected line by 1, properly handling wrapping.
  void SelectPreviousLine();

  // The user has choosen the selected line.
  bool AcceptSelectedLine();

  // The user has removed a suggestion.
  bool RemoveSelectedLine();

  // Convert a y-coordinate to the closest line.
  int LineFromY(int y);

  // Returns the height of a row depending on its type.
  int GetRowHeightFromId(int identifier) const;

  // Returns true if the given id refers to an element that can be accepted.
  bool CanAccept(int id);

  // Returns true if the popup still has non-options entries to show the user.
  bool HasSuggestions();

  // Set the Autofill entry values. Exposed to allow tests to set these values
  // without showing the popup.
  void SetValues(const std::vector<base::string16>& names,
                 const std::vector<base::string16>& subtexts,
                 const std::vector<base::string16>& icons,
                 const std::vector<int>& identifier);

  AutofillPopupView* view() { return view_; }

  // |view_| pass throughs (virtual for testing).
  virtual void ShowView();
  virtual void InvalidateRow(size_t row);

  // Protected so tests can access.
#if !defined(OS_ANDROID)
  // Calculates the desired width of the popup based on its contents.
  int GetDesiredPopupWidth() const;

  // Calculates the desired height of the popup based on its contents.
  int GetDesiredPopupHeight() const;

  // Calculate the width of the row, excluding all the text. This provides
  // the size of the row that won't be reducible (since all the text can be
  // elided if there isn't enough space).
  int RowWidthWithoutText(int row) const;
#endif

  base::WeakPtr<AutofillPopupControllerImpl> GetWeakPtr();

  // Contains common popup functionality such as popup layout. Protected for
  // testing.
  scoped_ptr<PopupControllerCommon> controller_common_;

 private:
  // Clear the internal state of the controller. This is needed to ensure that
  // when the popup is reused it doesn't leak values between uses.
  void ClearState();

#if !defined(OS_ANDROID)
  // Calculates and sets the bounds of the popup, including placing it properly
  // to prevent it from going off the screen.
  void UpdatePopupBounds();
#endif

  AutofillPopupView* view_;  // Weak reference.
  base::WeakPtr<AutofillPopupDelegate> delegate_;

  // The bounds of the Autofill popup.
  gfx::Rect popup_bounds_;

  // The text direction of the popup.
  base::i18n::TextDirection text_direction_;

  // The current Autofill query values.
  std::vector<base::string16> names_;
  std::vector<base::string16> subtexts_;
  std::vector<base::string16> icons_;
  std::vector<int> identifiers_;

  // Since names_ can be elided to ensure that it fits on the screen, we need to
  // keep an unelided copy of the names to be able to pass to the delegate.
  std::vector<base::string16> full_names_;

#if !defined(OS_ANDROID)
  // The fonts for the popup text.
  gfx::FontList name_font_list_;
  gfx::FontList subtext_font_list_;
  gfx::FontList warning_font_list_;
#endif

  // The line that is currently selected by the user.
  // |kNoSelection| indicates that no line is currently selected.
  int selected_line_;

  // Whether the popup view should hide on mouse presses outside of it.
  bool hide_on_outside_click_;

  base::WeakPtrFactory<AutofillPopupControllerImpl> weak_ptr_factory_;
};

}  // namespace autofill

#endif  // CHROME_BROWSER_UI_AUTOFILL_AUTOFILL_POPUP_CONTROLLER_IMPL_H_
