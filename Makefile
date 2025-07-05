build:
	g++ main.cpp -o gtk_sdl_video  `pkg-config --cflags --libs gtkmm-3.0 sdl2 gstreamer-1.0 gstreamer-app-1.0 gl stb`
clean:
	rm gtk_sdl_video
