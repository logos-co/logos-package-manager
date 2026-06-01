#pragma once

#include <iostream>
#include <sstream>
#include <string>

// RAII capture of std::cerr so tests can assert on emitted warning lines.
class CerrCapture {
public:
    CerrCapture() : m_old(std::cerr.rdbuf(m_buf.rdbuf())) {}
    ~CerrCapture() { std::cerr.rdbuf(m_old); }
    std::string str() const { return m_buf.str(); }
private:
    std::ostringstream m_buf;
    std::streambuf* m_old;
};
