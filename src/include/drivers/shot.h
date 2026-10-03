#ifndef SHOT_H
#define SHOT_H

// Screenshots: Print Screen saves what the panel shows as a BMP in
// /files/screenshots, named by the date and time. The key only asks
// (kbd.cpp, in an interrupt); the "shot" kernel process takes the picture
// and writes the file.

namespace shot
{
    // Start the kernel process. Once processes exist.
    void start();

    // Take a screenshot; fine from an interrupt.
    void request();
}

#endif // SHOT_H
