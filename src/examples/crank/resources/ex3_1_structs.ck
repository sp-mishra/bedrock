package ex3_1_structs

type Point = struct {
    x: Float32
    y: Float32
    z: Float32
}

fn Main() -> Float32 {
    let p = Point { 1.0, 2.0, 3.0 }
    let sum = p.x + p.y + p.z
    return sum
}
