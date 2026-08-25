package ex5_1_option

fn SafeDiv(a: Int32, b: Int32) -> Option[Int32] {
    if b == 0 {
        return Option.None
    }
    return Option.Some(a / b)
}

fn Main() -> Option[Int32] {
    return SafeDiv(10, 2)
}
