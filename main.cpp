#include <gtkmm.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <iostream>
#include <thread>
#include <atomic>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

class VideoApp : public Gtk::Window
{
public:
    VideoApp()
        : button_play("Start")
    {
        set_title("GStreamer GTKmm SDL2 Video Player");
        set_default_size(400, 200);

        // video file chooser
        vbox.pack_start(file_chooser, Gtk::PACK_SHRINK);
        file_chooser.set_title("Select video file");

        // fallback image file chooser
        vbox.pack_start(image_chooser, Gtk::PACK_SHRINK);
        image_chooser.set_title("Select fallback image");

        // SDL display list
        if (SDL_Init(SDL_INIT_VIDEO) < 0)
        {
            std::cerr << "SDL_Init error in GTK constructor: " << SDL_GetError() << "\n";
        }
        else
        {
            int num_displays = SDL_GetNumVideoDisplays();
            for (int i = 0; i < num_displays; ++i)
            {
                const char* name = SDL_GetDisplayName(i);
                SDL_Rect bounds;
                SDL_GetDisplayBounds(i, &bounds);

                std::ostringstream label;
                label << i << ": "
                      << (name ? name : "Unknown")
                      << " (" << bounds.w << "x" << bounds.h << ")";

                combo_display.append(label.str());
            }

            if (num_displays > 0)
                combo_display.set_active(0);

            // optional: keep SDL active for later
            // otherwise you could call SDL_Quit() here
            // but since you reuse SDL later it's OK to leave it
        }

        vbox.pack_start(combo_display, Gtk::PACK_SHRINK);

        // toggle play button
        vbox.pack_start(button_play, Gtk::PACK_SHRINK);
        button_play.signal_toggled().connect(sigc::mem_fun(*this, &VideoApp::on_play_toggled));

        add(vbox);
        show_all_children();

        gst_init(nullptr, nullptr);
    }

    ~VideoApp()
    {
        stop_playback = true;
        if (playback_thread.joinable())
            playback_thread.join();

        if (sdl_glcontext)
            SDL_GL_DeleteContext(sdl_glcontext);
        if (sdl_window)
            SDL_DestroyWindow(sdl_window);
        SDL_Quit();
    }

private:
    Gtk::Box vbox{Gtk::ORIENTATION_VERTICAL};
    Gtk::FileChooserButton file_chooser{"Choose video"};
    Gtk::ComboBoxText combo_display;
    Gtk::ToggleButton button_play;
    Gtk::FileChooserButton image_chooser{"Choose fallback image"};

    std::thread playback_thread;
    std::atomic<bool> stop_playback{false};

    // SDL persistent members
    SDL_Window* sdl_window = nullptr;
    SDL_GLContext sdl_glcontext = nullptr;

    void on_play_toggled()
    {
        if (button_play.get_active())
        {
            button_play.set_label("Stop");

            auto filename = file_chooser.get_filename();
            if (filename.empty())
            {
                std::cerr << "No file selected\n";
                button_play.set_active(false);
                return;
            }

            int display_index = combo_display.get_active_row_number();
            std::cout << "Selected display: " << display_index << "\n";

            stop_playback = false;
            playback_thread = std::thread([=]() {
                play_video(filename);
            });
        }
        else
        {
            button_play.set_label("Start");

            // request playback to stop
            if (playback_thread.joinable())
            {
                stop_playback = true;
                playback_thread.join();
            }
        }
    }

    void play_video(const std::string& filepath)
    {
        stop_playback = false;

        if (!sdl_window)
        {
            if (SDL_Init(SDL_INIT_VIDEO) < 0)
            {
                std::cerr << "SDL_Init error\n";
                return;
            }

            int display_index = combo_display.get_active_row_number();
            SDL_Rect display_bounds;
            SDL_GetDisplayBounds(display_index, &display_bounds);

            sdl_window = SDL_CreateWindow(
                "SDL2 OpenGL Video",
                display_bounds.x,
                display_bounds.y,
                display_bounds.w,
                display_bounds.h,
                SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

            if (!sdl_window)
            {
                std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
                SDL_Quit();
                return;
            }

            sdl_glcontext = SDL_GL_CreateContext(sdl_window);
            glEnable(GL_TEXTURE_2D);

            std::cout << "SDL window created in playback thread.\n";
        }

        // GStreamer pipeline
        GstElement* pipeline = gst_parse_launch(
            ("filesrc location=\"" + filepath + "\" ! decodebin ! videoconvert ! video/x-raw,format=RGB ! appsink name=sink").c_str(),
            nullptr);

        GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
        gst_element_set_state(pipeline, GST_STATE_PLAYING);

        GLuint texid = 0;
        bool is_fullscreen = true;
        bool first_frame = true;
        int video_width = 0;
        int video_height = 0;

        glGenTextures(1, &texid);
        glBindTexture(GL_TEXTURE_2D, texid);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        while (!stop_playback)
        {
            SDL_Event e;
            while (SDL_PollEvent(&e))
            {
                if (e.type == SDL_QUIT)
                {
                    stop_playback = true;
                    button_play.set_active(false);
                }
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_f)
                {
                    is_fullscreen = !is_fullscreen;
                    if (is_fullscreen)
                        SDL_SetWindowFullscreen(sdl_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                    else
                    {
                        SDL_SetWindowFullscreen(sdl_window, 0);
                        SDL_SetWindowSize(sdl_window, 800, 600);
                        SDL_SetWindowPosition(sdl_window, 100, 100);
                    }
                }
            }

            GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100000);
            if (sample)
            {
                GstBuffer* buffer = gst_sample_get_buffer(sample);
                GstCaps* caps = gst_sample_get_caps(sample);
                GstStructure* s = gst_caps_get_structure(caps, 0);

                gst_structure_get_int(s, "width", &video_width);
                gst_structure_get_int(s, "height", &video_height);

                GstMapInfo map;
                gst_buffer_map(buffer, &map, GST_MAP_READ);

                if (first_frame)
                {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                                 video_width, video_height, 0,
                                 GL_RGB, GL_UNSIGNED_BYTE, nullptr);
                    first_frame = false;
                }

                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                                video_width, video_height,
                                GL_RGB, GL_UNSIGNED_BYTE, map.data);

                gst_buffer_unmap(buffer, &map);
                gst_sample_unref(sample);

                int win_w, win_h;
                SDL_GetWindowSize(sdl_window, &win_w, &win_h);

                glViewport(0, 0, win_w, win_h);
                glClear(GL_COLOR_BUFFER_BIT);

                glBegin(GL_QUADS);
                    glTexCoord2f(0, 1); glVertex2f(-1.0f, -1.0f);
                    glTexCoord2f(1, 1); glVertex2f( 1.0f, -1.0f);
                    glTexCoord2f(1, 0); glVertex2f( 1.0f,  1.0f);
                    glTexCoord2f(0, 0); glVertex2f(-1.0f,  1.0f);
                glEnd();

                SDL_GL_SwapWindow(sdl_window);
            }
            else
            {
                SDL_Delay(10);
            }
        }

        // video playback done
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        glDeleteTextures(1, &texid);

        // fallback image (shown once)
        std::string imagepath = image_chooser.get_filename();
        if (!imagepath.empty())
        {
            int img_w, img_h, img_channels;
            unsigned char* img_data = stbi_load(imagepath.c_str(), &img_w, &img_h, &img_channels, 3);
            if (img_data)
            {
                GLuint fallback_texid;
                glGenTextures(1, &fallback_texid);
                glBindTexture(GL_TEXTURE_2D, fallback_texid);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                             img_w, img_h, 0,
                             GL_RGB, GL_UNSIGNED_BYTE, img_data);

                // draw once
                int win_w, win_h;
                SDL_GetWindowSize(sdl_window, &win_w, &win_h);

                glViewport(0, 0, win_w, win_h);
                glClear(GL_COLOR_BUFFER_BIT);

                glBegin(GL_QUADS);
                    glTexCoord2f(0, 1); glVertex2f(-1.0f, -1.0f);
                    glTexCoord2f(1, 1); glVertex2f( 1.0f, -1.0f);
                    glTexCoord2f(1, 0); glVertex2f( 1.0f,  1.0f);
                    glTexCoord2f(0, 0); glVertex2f(-1.0f,  1.0f);
                glEnd();

                SDL_GL_SwapWindow(sdl_window);

                stbi_image_free(img_data);
                glDeleteTextures(1, &fallback_texid);

                std::cout << "Fallback image displayed.\n";
            }
        }

        std::cout << "Playback finished, leaving SDL window open.\n";
    }
};

int main(int argc, char* argv[])
{
    auto app = Gtk::Application::create(argc, argv, "org.gtkmm.gstreamer.sdl2");
    VideoApp window;
    return app->run(window);
}
