//! The Universal C Runtime's `qsort` (`qsort.cpp`, unchanged since Visual
//! C++ 2003): a non-recursive quicksort with a median-of-three pivot that
//! stays in place while the partition runs, an explicit stack that always
//! defers the larger part, and a selection sort for parts of eight
//! elements or fewer. It is not stable; the order of elements that compare
//! equal is this algorithm's, and the reference's draw order depends on it
//! (the material batcher sorts its dispatch records and triangles with it).
//!
//! The comparison is the reference's callback, which need not be a
//! consistent order (NaN keys compare as greater in both directions); the
//! algorithm runs the same steps whatever it answers.

/// Parts this small are selection-sorted (`CUTOFF`).
const CUTOFF: usize = 8;

/// `qsort(base, num, width, compare)` on a slice: `compare(a, b)` answers
/// below, at or above 0 as `a` sorts before, with or after `b`.
pub fn qsort<T>(items: &mut [T], mut compare: impl FnMut(&T, &T) -> i32) {
    let num = items.len();
    if num < 2 {
        return;
    }
    let mut stack: Vec<(usize, usize)> = Vec::new();
    let mut lo = 0usize;
    let mut hi = num - 1;
    loop {
        let size = hi - lo + 1;
        if size <= CUTOFF {
            short_sort(items, lo, hi, &mut compare);
        } else {
            let mut mid = lo + size / 2;
            if compare(&items[lo], &items[mid]) > 0 {
                items.swap(lo, mid);
            }
            if compare(&items[lo], &items[hi]) > 0 {
                items.swap(lo, hi);
            }
            if compare(&items[mid], &items[hi]) > 0 {
                items.swap(mid, hi);
            }
            // Positions may step one past either end, as the C pointers do;
            // they are compared before they are dereferenced.
            let mut loguy = lo as isize;
            let mut higuy = hi as isize;
            let (lo_i, hi_i) = (lo as isize, hi as isize);
            loop {
                if (mid as isize) > loguy {
                    loop {
                        loguy += 1;
                        if !(loguy < mid as isize
                            && compare(&items[loguy as usize], &items[mid]) <= 0)
                        {
                            break;
                        }
                    }
                }
                if (mid as isize) <= loguy {
                    loop {
                        loguy += 1;
                        if !(loguy <= hi_i && compare(&items[loguy as usize], &items[mid]) <= 0) {
                            break;
                        }
                    }
                }
                loop {
                    higuy -= 1;
                    if !(higuy > mid as isize && compare(&items[higuy as usize], &items[mid]) > 0) {
                        break;
                    }
                }
                if higuy < loguy {
                    break;
                }
                items.swap(loguy as usize, higuy as usize);
                if mid as isize == higuy {
                    mid = loguy as usize;
                }
            }
            higuy += 1;
            if (mid as isize) < higuy {
                loop {
                    higuy -= 1;
                    if !(higuy > mid as isize && compare(&items[higuy as usize], &items[mid]) == 0)
                    {
                        break;
                    }
                }
            }
            if (mid as isize) >= higuy {
                loop {
                    higuy -= 1;
                    if !(higuy > lo_i && compare(&items[higuy as usize], &items[mid]) == 0) {
                        break;
                    }
                }
            }
            if higuy - lo_i >= hi_i - loguy {
                if lo_i < higuy {
                    stack.push((lo, higuy as usize));
                }
                if loguy < hi_i {
                    lo = loguy as usize;
                    continue;
                }
            } else {
                if loguy < hi_i {
                    stack.push((loguy as usize, hi));
                }
                if lo_i < higuy {
                    hi = higuy as usize;
                    continue;
                }
            }
        }
        match stack.pop() {
            Some((next_lo, next_hi)) => {
                lo = next_lo;
                hi = next_hi;
            }
            None => return,
        }
    }
}

/// `shortsort`: repeatedly moves the largest of `lo..=hi` (the last of
/// equal maxima found first) to the end.
fn short_sort<T>(
    items: &mut [T],
    lo: usize,
    mut hi: usize,
    compare: &mut impl FnMut(&T, &T) -> i32,
) {
    while hi > lo {
        let mut max = lo;
        for p in lo + 1..=hi {
            if compare(&items[p], &items[max]) > 0 {
                max = p;
            }
        }
        items.swap(max, hi);
        hi -= 1;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key_order(keys: &[i32]) -> Vec<usize> {
        let mut items: Vec<(i32, usize)> = keys.iter().copied().zip(0..).collect();
        qsort(&mut items, |a, b| a.0.cmp(&b.0) as i32);
        items.iter().map(|&(_, i)| i).collect()
    }

    #[test]
    fn sorts() {
        let mut values = [5, 3, 9, 1, 7, 2, 8, 6, 4, 0, 11, 10, 15, 13, 12, 14];
        qsort(&mut values, |a, b| a.cmp(b) as i32);
        assert_eq!(values, core::array::from_fn::<i32, 16, _>(|i| i as i32));
    }

    #[test]
    fn short_sort_moves_ties() {
        // Selection of the first maximum found, swapped to the end.
        assert_eq!(key_order(&[1, 1, 1]), [1, 2, 0]);
        assert_eq!(key_order(&[0, 0]), [1, 0]);
    }

    #[test]
    fn long_runs_of_ties() {
        let order = key_order(&[0; 12]);
        let mut sorted = order.clone();
        sorted.sort_unstable();
        assert_eq!(sorted, (0..12).collect::<Vec<_>>());
    }
}
