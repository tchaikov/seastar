template <class T>
class my_class {
public:
  my_class() {
    auto outer = [this]() {
      auto fn  = [this] {};
      // Use `fn` for something here.
    };
  }
};

int main() {
  my_class<int> r;
}
