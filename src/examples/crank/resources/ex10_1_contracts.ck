package ex10_1_contracts

fn SafeDiv(a: Int32, b: Int32) -> Int32
    requires b != 0
    ensures result == a / b
{
    return a / b
}

fn Main() -> Int32 {
    return SafeDiv(10, 2)
}
