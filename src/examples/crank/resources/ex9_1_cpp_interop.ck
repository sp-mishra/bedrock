package ex9_1_cpp_interop

// Example demonstrating C++ global/constant values used in Crank
// Simulates accessing C++ values within Crank computations

fn Main() -> Int64 {
    // Compute with fixed values to demonstrate C++ value usage pattern
    // In a real FFI scenario, these would be passed as external values
    let x: Int64 = 10   // Simulated value from C++
    let y: Int64 = 20   // Simulated value from C++

    // Compute using these "external" values
    let sum = x + y
    let product = x * y
    let max_val = if x > y { x } else { y }

    // Return result: 10 + 20 + (10*20) + 20 = 280
    return sum + product + max_val
}
