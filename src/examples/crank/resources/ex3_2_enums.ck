package ex3_2_enums

type Color = enum {
    Red
    Green
    Blue
}

fn Name(c: Color) -> String {
    match c {
        Color.Red => "red"
        Color.Green => "green"
        Color.Blue => "blue"
    }
}

fn Main() -> String {
    let c = Color.Red
    return Name(c)
}
