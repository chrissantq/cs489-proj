# CS 48900 - Embedded Systems Project
---------------------------------------------------
Christopher Quinney, Shafer Hofmann, James, Il<br>

## Files
---------------------------------------------------
cs489-proj.ino - The <br>
<br>
voice_server.cpp - The server implementation to parse commands that the arduino records. It implements the whisper.cpp library with the audio data received from the arduino to parse the speech into text. From there the server sends the text back to the arduino for action.<br>
<br>
whisper.cpp - A library developed by OpenAI for speech recognition<br>
