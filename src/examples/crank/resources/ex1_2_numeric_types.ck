package ex1_2_numeric_types

fn Main() -> Int32 {
    let i8: Int8 = 127
    let i16: Int16 = 32000
    let i32: Int32 = 2147483647
    let i64: Int64 = 9223372036854775807

    let u8: UInt8 = 255
    let u16: UInt16 = 65535
    let u32: UInt32 = 4294967295
    let u64: UInt64 = 18446744073709551615

    let f32: Float32 = 3.14
    let f64: Float64 = 3.141592653589793

    let max_i32: Int32 = 2147483647
    let wrapped: Int32 = max_i32 + 1

    return 1
}
