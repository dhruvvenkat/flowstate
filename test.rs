use std::collections::HashMap;

#[derive(Debug, Clone)]
struct Widget<'a> {
    name: &'a str,
    retries: usize,
}

impl<'a> Widget<'a> {
    pub fn render_value(input: i32) -> String {
        let mut values: HashMap<&str, i32> = HashMap::new();
        values.insert("answer", input + 0x2A);

        println!("testing flowstate: {:?}", values);
        format!(r#"widget={}"#, input)
    }
}

fn main() {
    let widget = Widget {
        name: "flowstate",
        retries: 3usize,
    };

    match Widget::render_value(42) {
        rendered if !rendered.is_empty() => println!("{:?}: {}", widget, rendered),
        _ => eprintln!("nothing rendered"),
    }
}
