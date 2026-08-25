package hello

fn Greet(name: String) -> String {
    return "Hello, " + name
}

fn Main() -> String {
    return Greet("World")
}
