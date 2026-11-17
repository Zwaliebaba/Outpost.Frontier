//! The VC6 C++ library and MSVCRT algorithms whose exact steps the data
//! depends on: `std::sort` (whose result depends on its steps when the
//! comparator is not a strict weak order) and `bsearch`.

/// The VC6 `std::sort` threshold (`_ISort_max`).
const SORT_MAX: usize = 16;

/// MSVCRT `bsearch` over `slots`, `compare` giving the key against a slot.
pub fn bsearch<T>(slots: &[T], compare: impl Fn(&T) -> i32) -> Option<usize> {
    let mut num = slots.len();
    if num == 0 {
        return None;
    }
    let mut lo = 0isize;
    let mut hi = isize::try_from(num).ok()? - 1;
    while lo <= hi {
        let half = num / 2;
        if half != 0 {
            let step = if num & 1 != 0 { half } else { half - 1 };
            let mid = lo + isize::try_from(step).ok()?;
            let index = usize::try_from(mid).ok()?;
            let result = compare(&slots[index]);
            if result == 0 {
                return Some(index);
            } else if result < 0 {
                hi = mid - 1;
                num = step;
            } else {
                lo = mid + 1;
                num = half;
            }
        } else if num != 0 {
            let index = usize::try_from(lo).ok()?;
            return (compare(&slots[index]) == 0).then_some(index);
        } else {
            break;
        }
    }
    None
}

/// The VC6 `std::sort` (`_Sort_0`, `_Sort`, `_Unguarded_partition`,
/// `_Median`, `_Insertion_sort`, `_Unguarded_insert`) step for step, with
/// `before` as `operator<`. The unguarded scans stop at the slice ends: the
/// C++ would read past them, which a comparator that is not a strict weak
/// order can make happen (see D150).
pub fn vc6_sort<T: Clone>(v: &mut [T], before: impl Fn(&T, &T) -> bool + Copy) {
    let len = v.len();
    if len <= SORT_MAX {
        insertion_sort(v, before);
        return;
    }
    sort_range(v, before);
    insertion_sort(&mut v[..SORT_MAX], before);
    for index in SORT_MAX..len {
        let value = v[index].clone();
        unguarded_insert(v, index, value, before);
    }
}

fn sort_range<T: Clone>(v: &mut [T], before: impl Fn(&T, &T) -> bool + Copy) {
    let (mut first, mut last) = (0usize, v.len());
    while last - first > SORT_MAX {
        let pivot = median(
            &v[first],
            &v[first + (last - first) / 2],
            &v[last - 1],
            before,
        )
        .clone();
        let mid = unguarded_partition(v, first, last, &pivot, before);
        if last - mid <= mid - first {
            sort_range(&mut v[mid..last], before);
            last = mid;
        } else {
            sort_range(&mut v[first..mid], before);
            first = mid;
        }
    }
}

fn median<'a, T>(x: &'a T, y: &'a T, z: &'a T, before: impl Fn(&T, &T) -> bool) -> &'a T {
    if before(x, y) {
        if before(y, z) {
            y
        } else if before(x, z) {
            z
        } else {
            x
        }
    } else if before(x, z) {
        x
    } else if before(y, z) {
        z
    } else {
        y
    }
}

fn unguarded_partition<T: Clone>(
    v: &mut [T],
    mut first: usize,
    mut last: usize,
    pivot: &T,
    before: impl Fn(&T, &T) -> bool,
) -> usize {
    loop {
        while first + 1 < v.len() && before(&v[first], pivot) {
            first += 1;
        }
        loop {
            if last == 0 {
                break;
            }
            last -= 1;
            if last == 0 || !before(pivot, &v[last]) {
                break;
            }
        }
        if last <= first {
            return first;
        }
        v.swap(first, last);
        first += 1;
    }
}

fn insertion_sort<T: Clone>(v: &mut [T], before: impl Fn(&T, &T) -> bool + Copy) {
    for index in 1..v.len() {
        let value = v[index].clone();
        if before(&value, &v[0]) {
            v[..=index].rotate_right(1);
        } else {
            unguarded_insert(v, index, value, before);
        }
    }
}

fn unguarded_insert<T: Clone>(
    v: &mut [T],
    mut last: usize,
    value: T,
    before: impl Fn(&T, &T) -> bool,
) {
    while last > 0 && before(&value, &v[last - 1]) {
        v[last] = v[last - 1].clone();
        last -= 1;
    }
    v[last] = value;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bsearch_finds_in_sorted_order() {
        let v: Vec<i32> = (0..37).map(|x| x * 3).collect();
        for (index, &x) in v.iter().enumerate() {
            assert_eq!(bsearch(&v, |&s| x - s), Some(index));
            assert_eq!(bsearch(&v, |&s| x + 1 - s), None);
        }
        assert_eq!(bsearch::<i32>(&[], |_| 0), None);
    }

    #[test]
    fn vc6_sort_sorts_a_strict_order() {
        let mut state = 12345u32;
        let mut v: Vec<u32> = (0..300)
            .map(|_| {
                state = state.wrapping_mul(1_103_515_245).wrapping_add(12345);
                state >> 8
            })
            .collect();
        let mut want = v.clone();
        want.sort_unstable();
        vc6_sort(&mut v, |a, b| a < b);
        assert_eq!(v, want);
    }
}
