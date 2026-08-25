package math_utils

fn Factorial(n: Int32) -> Int32 {
    if n <= 1 { return 1 }
    return n * Factorial(n - 1)
}

fn Fibonacci(n: Int32) -> Int32 {
    if n <= 1 { return n }
    return Fibonacci(n - 1) + Fibonacci(n - 2)
}

fn IsPrime(n: Int32) -> Bool {
    if n < 2 { return false }
    var i: Int32 = 2
    while i * i <= n {
        if n % i == 0 { return false }
        i = i + 1
    }
    return true
}
