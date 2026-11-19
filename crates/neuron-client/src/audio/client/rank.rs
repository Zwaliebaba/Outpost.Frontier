//! The mixer's ranking: the VC6 `std::sort` the client's sound tick runs
//! over its audible sources by score (`MixerSortRanked` and its helpers in
//! `CppSource/OutpostFrontier/Client/menu_sound_arch.cpp`), ported step for
//! step so that sources of equal score keep the reference's order.

/// `MixerRanksBefore`: `a` goes before `b` when `b`'s score is below `a`'s
/// (a NaN compares false).
fn before(scores: &dyn Fn(u32) -> f32, a: u32, b: u32) -> bool {
    scores(b) < scores(a)
}

/// `_Unguarded_insert` of the value at `last`.
fn unguarded_insert(keys: &mut [u32], last: usize, value: u32, scores: &dyn Fn(u32) -> f32) {
    let mut hole = last;
    let mut prev = last;
    loop {
        prev -= 1;
        if !before(scores, value, keys[prev]) {
            break;
        }
        keys[hole] = keys[prev];
        hole = prev;
    }
    keys[hole] = value;
}

/// `_Insertion_sort` over `keys[first..last]`.
fn insertion_sort(keys: &mut [u32], first: usize, last: usize, scores: &dyn Fn(u32) -> f32) {
    if first == last {
        return;
    }
    for mid in first + 1..last {
        let value = keys[mid];
        if before(scores, value, keys[first]) {
            keys.copy_within(first..mid, first + 1);
            keys[first] = value;
        } else {
            unguarded_insert(keys, mid, value, scores);
        }
    }
}

/// `_Sort`: partitions around the median of three while more than 16
/// remain, recursing into the part that is not larger.
fn intro_sort(keys: &mut [u32], mut first: usize, mut last: usize, scores: &dyn Fn(u32) -> f32) {
    while last - first > 16 {
        let count = last - first;
        let (a, b, c) = (keys[first], keys[first + count / 2], keys[last - 1]);
        let pivot = if before(scores, a, b) {
            if before(scores, b, c) {
                b
            } else if before(scores, a, c) {
                c
            } else {
                a
            }
        } else if before(scores, a, c) {
            a
        } else if before(scores, b, c) {
            c
        } else {
            b
        };
        let mut lo = first;
        let mut hi = last;
        loop {
            while before(scores, keys[lo], pivot) {
                lo += 1;
            }
            loop {
                hi -= 1;
                if !before(scores, pivot, keys[hi]) {
                    break;
                }
            }
            if hi <= lo {
                break;
            }
            keys.swap(lo, hi);
            lo += 1;
        }
        let mid = lo;
        if last - mid > mid - first {
            intro_sort(keys, first, mid, scores);
            first = mid;
        } else {
            intro_sort(keys, mid, last, scores);
            last = mid;
        }
    }
}

/// `MixerSortRanked`: highest score first, in the reference's order for
/// equal scores.
pub fn sort_ranked(keys: &mut [u32], scores: &dyn Fn(u32) -> f32) {
    let n = keys.len();
    if n > 16 {
        intro_sort(keys, 0, n, scores);
        insertion_sort(keys, 0, 16, scores);
        for it in 16..n {
            let value = keys[it];
            unguarded_insert(keys, it, value, scores);
        }
    } else {
        insertion_sort(keys, 0, n, scores);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn highest_first_and_ties_in_order() {
        let scores = [1.0_f32, 5.0, 3.0, 5.0, 2.0];
        let lookup = |k: u32| scores[k as usize];
        let mut keys = vec![0, 1, 2, 3, 4];
        sort_ranked(&mut keys, &lookup);
        assert_eq!(keys, [1, 3, 2, 4, 0]);
        let many: Vec<f32> = (0..40).map(|i| ((i * 37) % 23) as f32).collect();
        let lookup = |k: u32| many[k as usize];
        let mut keys: Vec<u32> = (0..40).collect();
        sort_ranked(&mut keys, &lookup);
        assert!(
            keys.windows(2)
                .all(|w| many[w[0] as usize] >= many[w[1] as usize])
        );
    }
}
