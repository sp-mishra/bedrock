package ex8_3_linked_list

type Node[T] = struct {
    value: T
    next: Option[*Node[T]]
}

type LinkedList[T] = struct {
    head: Option[*Node[T]]
}

fn Main() -> Int32 {
    return 1
}
