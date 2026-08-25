package ex4_1_basic_generics

fn Swap[T](a: T, b: T) -> (T, T) {
    return (b, a)
}

fn First[T](xs: []T) -> T {
    return xs[0]
}

fn Main() -> Int32 {
    let vals: [3]Int32 = [10, 20, 30]
    return First(vals[:])
}
