# Shake Ball

A magic 8-ball for the round screen. Ask a question
out loud, then shake the gadget: the ball rattles for
a moment and the answer window fills with the classic
blue triangle and one of the twenty traditional
answers. The motion sensor listens for a hard swing
of acceleration; a tap on the ball works too, for the
gentle. The number of shakes is kept in the app store.

One 400x400 canvas at zoom 1 in partial mode. The
scene repaints on state changes only; while the ball
rattles, each frame covers its old spot and draws it
at a jittered one, and an idle ball paints nothing.
