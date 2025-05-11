struct Human {
  unsigned age;
  unsigned height;
  virtual void sleep() = 0;
  virtual void eat() = 0;
};

struct Engineer : Human {
  unsigned salary;
  void sleep() override { /* something */ }
  void eat() override { /* something */ }
  void work() { /* something */ }
};

int main() {
	return 0;
}
