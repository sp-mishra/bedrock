package ex5_2_result

fn ParseInt(s: String) -> Result[Int32, String] {
    if len(s) == 0 {
        return Result.Err("empty string")
    }
    var sum: Int32 = 0
    for i := range 0..len(s) {
        let c = s[i]
        if c < '0' || c > '9' {
            return Result.Err("non-digit")
        }
        sum = sum * 10 + (c - '0')
    }
    return Result.Ok(sum)
}

fn Main() -> Result[Int32, String] {
    return ParseInt("42")
}
