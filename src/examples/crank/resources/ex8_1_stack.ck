package ex8_1_stack

type Stack[T] = struct {
    items: []T
    size: UInt64
}

fn Main() -> Int32 {
    var s: Stack[Int32] = Stack { items: [], size: 0 }
    return 1
}
