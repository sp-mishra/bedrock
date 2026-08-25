package ex1_1_basic_types

fn Add(a: Int32, b: Int32) -> Int32 {
    return a + b
}

fn IsPositive(n: Int32) -> Bool {
    return n > 0
}

fn Main() -> Int32 {
    let result1 = Add(3, 4)
    let result2 = IsPositive(5)
    return result1
}
