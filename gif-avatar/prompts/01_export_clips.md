I'm putting your real avatar animations on my desk gadget (the round ESP32 screen you're connected to), in place of the generic character it shows now. Please package your avatar clips for me. Only create new files: don't modify or delete anything in your avatars folder, and don't paste any file contents into your reply.

1. Find your active avatar: read workspace/avatars/avatar-state.json (or your /identity avatar fields) for the active stem.
2. Make the folder workspace/gadget-export/ (empty it first if it already exists).
3. Copy your active avatar's dark-mode video clips into it, renamed to the animation name alone:
   - the dark-mode idle clip as idle.mp4 (required; if you have no dark-mode version, use the normal one)
   - and each of these that exists: waking, working, thinking, making_something, milestone_level_up, listening, speaking, error, off (for example STEM-darkmode-working.mp4 becomes working.mp4)
   Keep the original files untouched; just copy. Don't convert, crop or resize anything.
4. Zip only those MP4s as workspace/gadget-export/avatar-clips.zip.
5. Attach avatar-clips.zip to our chat in the Muse app so I can save it from my phone.
6. Reply with a short plain-text list:
   - the active stem
   - each MP4 in the zip: name, resolution, frame rate, duration and size in bytes
   - the zip's size in bytes and sha256
   - whether you attached it (yes or no); if you can't attach files, say so plainly
