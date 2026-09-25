// tests/test_bug_line_number.c
// Bug: parser.c stamps ND_IF/ND_WHILE/ND_BINOP node->line via newNode()
// AFTER parsing children (lexer_current_line() called post-consumption).
// node->line ends up = last line of the branch/operand, not the
// statement's own starting line.

int main() {
    int x;
    x = 1;
    if (x == 1)
    {
        x = 2;
        x = 3;
        x = 4;
    }
    return x;
}