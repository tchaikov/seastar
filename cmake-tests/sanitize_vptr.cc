struct A {
    virtual ~A() {}
};

struct B : virtual A {};
struct C : virtual A {};
struct D : V, virtual C {};

int main() {
    D d;
}
