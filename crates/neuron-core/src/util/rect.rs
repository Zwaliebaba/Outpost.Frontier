//! `Rect`, `ViewRect` and their conversions (`common_rect.cpp`).
//!
//! A [`Rect`] is an origin and an extent; [`ViewRect`] holds inclusive
//! edges. The C++ uses plain `int` arithmetic, whose overflow is undefined;
//! here it wraps, which is what the reference's instructions do.

/// `Rect`: left, top, width and height.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct Rect {
    /// `Left`.
    pub left: i32,
    /// `Top`.
    pub top: i32,
    /// `Width`.
    pub width: i32,
    /// `Height`.
    pub height: i32,
}

/// `ViewRect`: inclusive edges, `right = left + width - 1`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct ViewRect {
    /// `Left`.
    pub left: i32,
    /// `Top`.
    pub top: i32,
    /// `Right`, inclusive.
    pub right: i32,
    /// `Bottom`, inclusive.
    pub bottom: i32,
}

/// The Windows `RECT` as `Rect_to_RECT` fills it. The C++ subtracts 2, not
/// 1, from the far edges, and `RECT_to_Rect` adds 2 back.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct WinRect {
    /// `left`.
    pub left: i32,
    /// `top`.
    pub top: i32,
    /// `right`.
    pub right: i32,
    /// `bottom`.
    pub bottom: i32,
}

impl Rect {
    /// `Rect(left, top, width, height)`.
    #[must_use]
    pub const fn new(left: i32, top: i32, width: i32, height: i32) -> Self {
        Self {
            left,
            top,
            width,
            height,
        }
    }

    /// `init_from_ltrb`: inclusive edges to extents.
    #[must_use]
    pub const fn from_ltrb(left: i32, top: i32, right: i32, bottom: i32) -> Self {
        Self {
            left,
            top,
            width: right.wrapping_sub(left).wrapping_add(1),
            height: bottom.wrapping_sub(top).wrapping_add(1),
        }
    }

    /// `clear`.
    pub fn clear(&mut self) {
        *self = Self::default();
    }

    /// `rightmost`: `left + width - 1`.
    #[must_use]
    pub const fn rightmost(&self) -> i32 {
        self.left.wrapping_add(self.width).wrapping_sub(1)
    }

    /// `bottommost`: `top + height - 1`.
    #[must_use]
    pub const fn bottommost(&self) -> i32 {
        self.top.wrapping_add(self.height).wrapping_sub(1)
    }

    /// `area`: `width * height`.
    #[must_use]
    pub const fn area(&self) -> i32 {
        self.width.wrapping_mul(self.height)
    }

    /// `null`: the width or the height is zero.
    #[must_use]
    pub const fn is_null(&self) -> bool {
        self.width == 0 || self.height == 0
    }

    /// `nonnull`: the width and the height are positive. A rectangle with a
    /// negative extent is neither null nor non-null.
    #[must_use]
    pub const fn is_nonnull(&self) -> bool {
        self.width > 0 && self.height > 0
    }

    /// `get_center`: `(2 * left + width - 1) / 2`, truncating, and the same
    /// for y.
    #[must_use]
    pub const fn center(&self) -> (i32, i32) {
        let x = self
            .left
            .wrapping_mul(2)
            .wrapping_add(self.width)
            .wrapping_sub(1);
        let y = self
            .top
            .wrapping_mul(2)
            .wrapping_add(self.height)
            .wrapping_sub(1);
        (x.wrapping_div(2), y.wrapping_div(2))
    }

    /// `point_is_inside`: `left <= x < left + width`, and the same for y.
    #[must_use]
    pub const fn contains(&self, x: i32, y: i32) -> bool {
        x >= self.left
            && x < self.left.wrapping_add(self.width)
            && y >= self.top
            && y < self.top.wrapping_add(self.height)
    }

    /// `Rect_to_ViewRect`.
    #[must_use]
    pub const fn to_view(&self) -> ViewRect {
        ViewRect {
            left: self.left,
            top: self.top,
            right: self.width.wrapping_add(self.left).wrapping_sub(1),
            bottom: self.height.wrapping_add(self.top).wrapping_sub(1),
        }
    }

    /// `ViewRect_to_Rect`.
    #[must_use]
    pub const fn from_view(view: ViewRect) -> Self {
        Self {
            left: view.left,
            top: view.top,
            width: view.right.wrapping_sub(view.left).wrapping_add(1),
            height: view.bottom.wrapping_sub(view.top).wrapping_add(1),
        }
    }

    /// `Rect_to_RECT`: the far edges are `left + width - 2`.
    #[must_use]
    pub const fn to_win(&self) -> WinRect {
        WinRect {
            left: self.left,
            top: self.top,
            right: self.left.wrapping_add(self.width).wrapping_sub(2),
            bottom: self.top.wrapping_add(self.height).wrapping_sub(2),
        }
    }

    /// `RECT_to_Rect`: the extents are `right - left + 2`.
    #[must_use]
    pub const fn from_win(win: WinRect) -> Self {
        Self {
            left: win.left,
            top: win.top,
            width: win.right.wrapping_sub(win.left).wrapping_add(2),
            height: win.bottom.wrapping_sub(win.top).wrapping_add(2),
        }
    }

    /// `operator==`: all fields equal, or both null.
    #[must_use]
    pub const fn same_as(&self, other: &Self) -> bool {
        let fields = self.left == other.left
            && self.top == other.top
            && self.width == other.width
            && self.height == other.height;
        fields || (self.is_null() && other.is_null())
    }

    /// `operator!=`: some field differs and one side is non-null. Not the
    /// negation of [`same_as`](Self::same_as) when an extent is negative.
    #[must_use]
    pub const fn differs_from(&self, other: &Self) -> bool {
        let fields = self.left != other.left
            || self.top != other.top
            || self.width != other.width
            || self.height != other.height;
        fields && (self.is_nonnull() || other.is_nonnull())
    }

    /// `rect_intersect`: the overlap, or the zero rectangle when the edges do
    /// not overlap.
    #[must_use]
    pub const fn intersect(&self, other: &Self) -> Self {
        let a_right = self.left.wrapping_sub(1).wrapping_add(self.width);
        let b_right = other.left.wrapping_sub(1).wrapping_add(other.width);
        let left = if self.left >= other.left {
            self.left
        } else {
            other.left
        };
        let right = if a_right <= b_right { a_right } else { b_right };
        if left <= right {
            let a_bottom = self.top.wrapping_sub(1).wrapping_add(self.height);
            let b_bottom = other.top.wrapping_sub(1).wrapping_add(other.height);
            let top = if self.top >= other.top {
                self.top
            } else {
                other.top
            };
            let bottom = if a_bottom <= b_bottom {
                a_bottom
            } else {
                b_bottom
            };
            if top <= bottom {
                return Self::from_ltrb(left, top, right, bottom);
            }
        }
        Self::new(0, 0, 0, 0)
    }

    /// `rect_union`: the bounding rectangle. A side with a zero extent is
    /// ignored (`self` null gives `other`, `other` null gives `self`).
    #[must_use]
    pub const fn union(&self, other: &Self) -> Self {
        if self.is_null() {
            return *other;
        }
        if other.is_null() {
            return *self;
        }
        let a_right = self.left.wrapping_sub(1).wrapping_add(self.width);
        let a_bottom = self.top.wrapping_sub(1).wrapping_add(self.height);
        let b_right = other.left.wrapping_sub(1).wrapping_add(other.width);
        let b_bottom = other.top.wrapping_sub(1).wrapping_add(other.height);
        let left = if self.left < other.left {
            self.left
        } else {
            other.left
        };
        let right = if a_right > b_right { a_right } else { b_right };
        let top = if self.top < other.top {
            self.top
        } else {
            other.top
        };
        let bottom = if a_bottom > b_bottom {
            a_bottom
        } else {
            b_bottom
        };
        Self::from_ltrb(left, top, right, bottom)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn center_uses_the_inclusive_edges() {
        assert_eq!(Rect::new(0, 0, 4, 4).center(), (1, 1));
        assert_eq!(Rect::new(0, 0, 5, 5).center(), (2, 2));
        assert_eq!(Rect::new(-3, -3, 0, 0).center(), (-3, -3));
    }

    #[test]
    fn equality_quirks() {
        let a = Rect::new(1, 2, 0, 5);
        let b = Rect::new(9, 9, 3, 0);
        assert!(a.same_as(&b));
        assert!(!a.differs_from(&b));
        let c = Rect::new(1, 2, -1, -1);
        let d = Rect::new(3, 4, -1, -1);
        assert!(!c.same_as(&d));
        assert!(!c.differs_from(&d));
    }

    #[test]
    fn intersect_and_union() {
        let a = Rect::new(0, 0, 10, 10);
        let b = Rect::new(5, 5, 10, 10);
        assert_eq!(a.intersect(&b), Rect::new(5, 5, 5, 5));
        assert_eq!(a.union(&b), Rect::new(0, 0, 15, 15));
        assert_eq!(a.intersect(&Rect::new(20, 0, 1, 1)), Rect::default());
        assert_eq!(Rect::new(3, 3, 0, 1).union(&b), b);
    }

    #[test]
    fn conversions() {
        let r = Rect::new(10, 20, 30, 40);
        assert_eq!(
            r.to_view(),
            ViewRect {
                left: 10,
                top: 20,
                right: 39,
                bottom: 59
            }
        );
        assert_eq!(Rect::from_view(r.to_view()), r);
        assert_eq!(
            r.to_win(),
            WinRect {
                left: 10,
                top: 20,
                right: 38,
                bottom: 58
            }
        );
        assert_eq!(Rect::from_win(r.to_win()), r);
        assert_eq!(Rect::from_ltrb(10, 20, 39, 59), r);
    }
}
