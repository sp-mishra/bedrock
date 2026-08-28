package ex4_2_trait_bounds

trait Comparable {
    fn Less(a: Self, b: Self) -> Bool
}

impl Comparable for Int32 {
    fn Less(a: Int32, b: Int32) -> Bool {
        return a < b
    }
}

fn Min[T: Comparable](xs: []T) -> T {
    var min = xs[0]
    for i := range 1..len(xs) {
        if T.Less(xs[i], min) {
            min = xs[i]
        }
    }
    return min
}

fn Main() -> Int32 {
    let vals: [5]Int32 = [5, 2, 8, 1, 9]
    return Min(vals[:])
}
