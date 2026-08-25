package ex2_3_vector

// Example demonstrating dynamic collection operations similar to C++ vector
// In Crank, dynamic collections are represented through slices and arrays

fn Main() -> Int32 {
    // Create a fixed-size array (like std::vector in C++ but pre-allocated)
    let arr: [10]Int32 = [1, 2, 3, 4, 5, 0, 0, 0, 0, 0]

    // Create a slice representing the used portion (like std::vector.data())
    let slice: []Int32 = arr[0..5]

    // Compute operations on the collection
    var sum: Int32 = 0
    var product: Int32 = 1

    for elem := range slice {
        sum += elem
        product *= elem
    }

    // Demonstrate indexing (std::vector like access)
    var result: Int32 = 0
    if len(slice) > 0 {
        result = slice[0] + slice[len(slice) - 1]  // first + last
    }

    // Sum of all elements: 1+2+3+4+5 = 15
    return sum
}
