package data_pipeline

type Record = struct {
    id: Int32
    value: Float32
}

fn SumValues(records: []Record) -> Float32 {
    var total: Float32 = 0.0
    for i := range 0..len(records) {
        total = total + records[i].value
    }
    return total
}

fn FilterPositive(records: []Record) -> Int32 {
    var count: Int32 = 0
    for i := range 0..len(records) {
        if records[i].value > 0.0 { count = count + 1 }
    }
    return count
}
