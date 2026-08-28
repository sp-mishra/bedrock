package ex2_1_collections

fn Main() -> Int32 {
    let arr: [5]Int32 = [1, 2, 3, 4, 5]
    let len_arr = len(arr)

    let slice: []Int32 = arr[0..3]
    let len_slice = len(slice)

    var sum: Int32 = 0
    for elem := range slice {
        sum += elem
    }

    return sum
}
