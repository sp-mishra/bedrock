package ex1_3_control_flow

fn Classify(n: Int32) -> String {
    if n < 0 {
        return "negative"
    } else if n == 0 {
        return "zero"
    } else {
        return "positive"
    }
}

fn Sum(xs: []Int32) -> Int32 {
    var sum: Int32 = 0
    for i := range 0..len(xs) {
        sum += xs[i]
    }
    return sum
}

fn Main() -> Int32 {
    let vals: [5]Int32 = [1, 2, 3, 4, 5]
    return Sum(vals[:])
}
