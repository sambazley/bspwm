#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#include <xcb/xcb_icccm.h>
#include "types.h"
#include "bspwm.h"
#include "settings.h"
#include "helpers.h"
#include "window.h"
#include "events.h"
#include "tree.h"
#include "rules.h"
#include "ewmh.h"

void handle_event(xcb_generic_event_t *evt)
{
    switch (XCB_EVENT_RESPONSE_TYPE(evt)) {
        case XCB_MAP_REQUEST:
            map_request(evt);
            break;
        case XCB_DESTROY_NOTIFY:
            destroy_notify(evt);
            break;
        case XCB_UNMAP_NOTIFY:
            unmap_notify(evt);
            break;
        case XCB_CLIENT_MESSAGE:
            client_message(evt);
            break;
        case XCB_CONFIGURE_REQUEST:
            configure_request(evt);
            break;
        case XCB_PROPERTY_NOTIFY:
            property_notify(evt);
            break;
        case XCB_ENTER_NOTIFY:
            enter_notify(evt);
            break;
        case XCB_MOTION_NOTIFY:
            motion_notify(evt);
            break;
        default:
            break;
    }
}

void map_request(xcb_generic_event_t *evt)
{
    xcb_map_request_event_t *e = (xcb_map_request_event_t *) evt;

    PRINTF("map request %X\n", e->window);

    manage_window(mon, mon->desk, e->window);
}

void configure_request(xcb_generic_event_t *evt)
{
    xcb_configure_request_event_t *e = (xcb_configure_request_event_t *) evt;

    PRINTF("configure request %X\n", e->window);

    window_location_t loc;
    bool is_managed = locate_window(e->window, &loc);

    if (!is_managed || is_floating(loc.node->client)) {
        uint16_t mask = 0;
        uint32_t values[7];
        unsigned short i = 0;

        if (e->value_mask & XCB_CONFIG_WINDOW_X) {
            mask |= XCB_CONFIG_WINDOW_X;
            values[i++] = e->x;
            if (is_managed)
                loc.node->client->floating_rectangle.x = e->x;
        }

        if (e->value_mask & XCB_CONFIG_WINDOW_Y) {
            mask |= XCB_CONFIG_WINDOW_Y;
            values[i++] = e->y;
            if (is_managed)
                loc.node->client->floating_rectangle.y = e->y;
        }

        if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
            mask |= XCB_CONFIG_WINDOW_WIDTH;
            values[i++] = e->width;
            if (is_managed)
                loc.node->client->floating_rectangle.width = e->width;
        }

        if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
            mask |= XCB_CONFIG_WINDOW_HEIGHT;
            values[i++] = e->height;
            if (is_managed)
                loc.node->client->floating_rectangle.height = e->height;
        }

        if (!is_managed && e->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
            mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH;
            values[i++] = e->border_width;
        }

        if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
            mask |= XCB_CONFIG_WINDOW_SIBLING;
            values[i++] = e->sibling;
        }

        if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
            mask |= XCB_CONFIG_WINDOW_STACK_MODE;
            values[i++] = e->stack_mode;
        }

        xcb_configure_window(dpy, e->window, mask, values);
        if (is_managed)
            window_draw_border(loc.node, loc.node == loc.desktop->focus, loc.monitor == mon);
    } else {
        xcb_configure_notify_event_t evt;
        xcb_rectangle_t rect;
        unsigned int bw;
        xcb_window_t win = loc.node->client->window;

        if (is_tiled(loc.node->client)) {
            rect = loc.node->client->tiled_rectangle;
            bw = border_width;
        } else {
            rect = loc.monitor->rectangle;
            bw = 0;
        }

        evt.response_type = XCB_CONFIGURE_NOTIFY;
        evt.event = win;
        evt.window = win;
        evt.above_sibling = XCB_NONE;
        evt.x = rect.x;
        evt.y = rect.y;
        evt.width = rect.width;
        evt.height = rect.height;
        evt.border_width = bw;
        evt.override_redirect = false;

        xcb_send_event(dpy, false, win, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *) &evt);
    }
}

void destroy_notify(xcb_generic_event_t *evt)
{
    xcb_destroy_notify_event_t *e = (xcb_destroy_notify_event_t *) evt;

    PRINTF("destroy notify %X\n", e->window);

    window_location_t loc;
    if (locate_window(e->window, &loc)) {
        remove_node(loc.desktop, loc.node);
        arrange(loc.monitor, loc.desktop);
    }
}

void unmap_notify(xcb_generic_event_t *evt)
{
    xcb_unmap_notify_event_t *e = (xcb_unmap_notify_event_t *) evt;

    PRINTF("unmap notify %X\n", e->window);

    window_location_t loc;
    if (locate_window(e->window, &loc)) {
        remove_node(loc.desktop, loc.node);
        arrange(loc.monitor, loc.desktop);
    }
}

void property_notify(xcb_generic_event_t *evt)
{
    xcb_property_notify_event_t *e = (xcb_property_notify_event_t *) evt;
    xcb_icccm_wm_hints_t hints;

    /* PRINTF("property notify %X\n", e->window); */

    if (e->atom != XCB_ATOM_WM_HINTS)
        return;

    window_location_t loc;
    if (locate_window(e->window, &loc)) {
        if (xcb_icccm_get_wm_hints_reply(dpy, xcb_icccm_get_wm_hints(dpy, e->window), &hints, NULL) == 1) {
            uint32_t urgent = xcb_icccm_wm_hints_get_urgency(&hints);
            if (urgent != 0 && loc.node != mon->desk->focus) {
                loc.node->client->urgent = urgent;
                put_status();
                if (loc.monitor->desk == loc.desktop)
                    arrange(loc.monitor, loc.desktop);
            }
        }
    }
}

void client_message(xcb_generic_event_t *evt)
{
    xcb_client_message_event_t *e = (xcb_client_message_event_t *) evt;

    PRINTF("client message %X %u\n", e->window, e->type);

    if (e->type == ewmh->_NET_CURRENT_DESKTOP) {
        desktop_location_t loc;
        if (ewmh_locate_desktop(e->data.data32[0], &loc)) {
            select_monitor(loc.monitor);
            select_desktop(loc.desktop);
        }
        return;
    }

    window_location_t loc;
    if (!locate_window(e->window, &loc))
        return;

    if (e->type == ewmh->_NET_WM_STATE) {
        handle_state(loc.monitor, loc.desktop, loc.node, e->data.data32[1], e->data.data32[0]);
        handle_state(loc.monitor, loc.desktop, loc.node, e->data.data32[2], e->data.data32[0]);
    } else if (e->type == ewmh->_NET_ACTIVE_WINDOW) {
        if (loc.desktop->focus->client->fullscreen && loc.desktop->focus != loc.node)
            toggle_fullscreen(loc.monitor, loc.desktop->focus->client);
        select_monitor(loc.monitor);
        select_desktop(loc.desktop);
        focus_node(loc.monitor, loc.desktop, loc.node, true);
        arrange(loc.monitor, loc.desktop);
    }
}

void enter_notify(xcb_generic_event_t *evt)
{
    xcb_enter_notify_event_t *e = (xcb_enter_notify_event_t *) evt;
    xcb_window_t win = e->event;

    PRINTF("enter notify %X %d %d\n", win, e->mode, e->detail);

    if (e->mode != XCB_NOTIFY_MODE_NORMAL
            || (last_pointer_position.x = e->root_x && last_pointer_position.y == e->root_y))
        return;

    window_focus(win);
}

void motion_notify(xcb_generic_event_t *evt)
{
    xcb_motion_notify_event_t *e = (xcb_motion_notify_event_t *) evt;
    xcb_window_t win = e->event;

    PRINTF("motion notify %X\n", win);

    window_focus(win);
}

void handle_state(monitor_t *m, desktop_t *d, node_t *n, xcb_atom_t state, unsigned int action)
{
    if (state == ewmh->_NET_WM_STATE_FULLSCREEN) {
        bool fs = n->client->fullscreen;
        if (action == XCB_EWMH_WM_STATE_TOGGLE
                || (fs && action == XCB_EWMH_WM_STATE_REMOVE)
                || (!fs && action == XCB_EWMH_WM_STATE_ADD)) {
            toggle_fullscreen(m, n->client);
            arrange(m, d);
        }
    }
}

void grab_pointer(pointer_action_t pac)
{
    PRINTF("grab pointer %u\n", pac);

    xcb_window_t win;
    xcb_point_t pos;

    xcb_query_pointer_reply_t *qpr = xcb_query_pointer_reply(dpy, xcb_query_pointer(dpy, root), NULL);
    if (qpr != NULL) {
        pos = (xcb_point_t) {qpr->root_x, qpr->root_y};
        win = qpr->child;
        free(qpr);
    } else {
        return;
    }

    window_location_t loc;
    bool found_win = locate_window(win, &loc);
    if (found_win || pac == ACTION_RESIZE_TILED) {
        fence_distance_t fd;
        client_t *c = NULL;
        frozen_pointer->position = pos;
        frozen_pointer->action = pac;
        if (found_win) {
            c = loc.node->client;
            frozen_pointer->monitor = loc.monitor;
            frozen_pointer->desktop = loc.desktop;
            frozen_pointer->node = loc.node;
            frozen_pointer->client = c;
            frozen_pointer->window = c->window;
        }
        switch (pac)  {
            case ACTION_FOCUS:
                focus_node(loc.monitor, loc.desktop, loc.node, true);
                break;
            case ACTION_MOVE:
            case ACTION_RESIZE_SIDE:
            case ACTION_RESIZE_CORNER:
                if (is_tiled(c)) {
                    loc.node->client->floating_rectangle = c->tiled_rectangle;
                    toggle_floating(loc.node);
                    arrange(loc.monitor, loc.desktop);
                } else if (c->fullscreen) {
                    frozen_pointer->action = ACTION_NONE;
                    return;
                }
                frozen_pointer->rectangle = c->floating_rectangle;

                if (pac == ACTION_RESIZE_SIDE) {
                    float W = c->floating_rectangle.width;
                    float H = c->floating_rectangle.height;
                    float ratio = W / H;
                    float x = pos.x - c->floating_rectangle.x;
                    float y = pos.y - c->floating_rectangle.y;
                    float diag_a = ratio * y;
                    float diag_b = W - diag_a;

                    if (x < diag_a) {
                        if (x < diag_b)
                            frozen_pointer->side = SIDE_LEFT;
                        else
                            frozen_pointer->side = SIDE_BOTTOM;
                    } else {
                        if (x < diag_b)
                            frozen_pointer->side = SIDE_TOP;
                        else
                            frozen_pointer->side = SIDE_RIGHT;
                    }
                } else if (pac == ACTION_RESIZE_CORNER) {
                    int16_t mid_x, mid_y;
                    mid_x = c->floating_rectangle.x + (c->floating_rectangle.width / 2);
                    mid_y = c->floating_rectangle.y + (c->floating_rectangle.height / 2);
                    if (pos.x > mid_x) {
                        if (pos.y > mid_y)
                            frozen_pointer->corner = CORNER_BOTTOM_RIGHT;
                        else
                            frozen_pointer->corner = CORNER_TOP_RIGHT;
                    } else {
                        if (pos.y > mid_y)
                            frozen_pointer->corner = CORNER_BOTTOM_LEFT;
                        else
                            frozen_pointer->corner = CORNER_TOP_LEFT;
                    }
                }
                break;
            case ACTION_RESIZE_TILED:
                fd = nearest_fence(pos, mon->desk->root);
                if (fd.fence != NULL && fd.distance < fence_grip) {
                    frozen_pointer->node = fd.fence;
                    frozen_pointer->action = pac;
                    frozen_pointer->rectangle = fd.fence->rectangle;
                } else {
                    frozen_pointer->action = ACTION_NONE;
                }
                break;
            case ACTION_MOVE_TILED:
                if (!is_tiled(c))
                    frozen_pointer->action = ACTION_NONE;
                break;
            case ACTION_NONE:
                break;
        }
    } else {
        frozen_pointer->action = ACTION_NONE;
    }
}

void track_pointer(int root_x, int root_y)
{
    if (frozen_pointer->action == ACTION_NONE)
        return;

    int16_t delta_x, delta_y, x, y, w, h;
    uint16_t width, height;

    monitor_t *m = frozen_pointer->monitor;
    desktop_t *d = frozen_pointer->desktop;
    node_t *n = frozen_pointer->node;
    client_t *c = frozen_pointer->client;
    xcb_window_t win = frozen_pointer->window;
    xcb_rectangle_t rect = frozen_pointer->rectangle;

    x = y = 0;
    w = h = 1;

    delta_x = root_x - frozen_pointer->position.x;
    delta_y = root_y - frozen_pointer->position.y;
    double sr;
    xcb_query_pointer_reply_t *qpr;
    xcb_window_t pwin;
    window_location_t loc;

    switch (frozen_pointer->action) {
        case ACTION_MOVE:
            x = rect.x + delta_x;
            y = rect.y + delta_y;
            window_move(win, x, y);
            break;
        case ACTION_RESIZE_SIDE:
            switch (frozen_pointer->side) {
                case SIDE_TOP:
                    x = rect.x;
                    y = rect.y + delta_y;
                    w = rect.width;
                    h = rect.height - delta_y;
                    break;
                case SIDE_RIGHT:
                    x = rect.x;
                    y = rect.y;
                    w = rect.width + delta_x;
                    h = rect.height;
                    break;
                case SIDE_BOTTOM:
                    x = rect.x;
                    y = rect.y;
                    w = rect.width;
                    h = rect.height + delta_y;
                    break;
                case SIDE_LEFT:
                    x = rect.x + delta_x;
                    y = rect.y;
                    w = rect.width - delta_x;
                    h = rect.height;
                    break;
            }
            width = MAX(1, w);
            height = MAX(1, h);
            window_move_resize(win, x, y, width, height);
            c->floating_rectangle = (xcb_rectangle_t) {x, y, width, height};
            window_draw_border(n, d->focus == n, mon == m);
            break;
        case ACTION_RESIZE_CORNER:
            switch (frozen_pointer->corner) {
                case CORNER_TOP_LEFT:
                    x = rect.x + delta_x;
                    y = rect.y + delta_y;
                    w = rect.width - delta_x;
                    h = rect.height - delta_y;
                    break;
                case CORNER_TOP_RIGHT:
                    x = rect.x;
                    y = rect.y + delta_y;
                    w = rect.width + delta_x;
                    h = rect.height - delta_y;
                    break;
                case CORNER_BOTTOM_LEFT:
                    x = rect.x + delta_x;
                    y = rect.y;
                    w = rect.width - delta_x;
                    h = rect.height + delta_y;
                    break;
                case CORNER_BOTTOM_RIGHT:
                    x = rect.x;
                    y = rect.y;
                    w = rect.width + delta_x;
                    h = rect.height + delta_y;
                    break;
            }
            width = MAX(1, w);
            height = MAX(1, h);
            window_move_resize(win, x, y, width, height);
            c->floating_rectangle = (xcb_rectangle_t) {x, y, width, height};
            window_draw_border(n, d->focus == n, mon == m);
            break;
        case ACTION_RESIZE_TILED:
            if (n->split_type == TYPE_VERTICAL)
                sr = (double) (root_x - rect.x + window_gap / 2) / rect.width;
            else
                sr = (double) (root_y - rect.y + window_gap / 2) / rect.height;
            sr = MAX(0, sr);
            sr = MIN(1, sr);
            n->split_ratio = sr;
            arrange(mon, mon->desk);
            break;
        case ACTION_MOVE_TILED:
            qpr = xcb_query_pointer_reply(dpy, xcb_query_pointer(dpy, root), NULL);
            if (qpr != NULL) {
                pwin = qpr->child;
                free(qpr);
                if (locate_window(pwin, &loc) && loc.monitor == m && loc.desktop == d && is_tiled(loc.node->client)) {
                    swap_nodes(n, loc.node);
                    arrange(m, d);
                }
            }
            break;
        case ACTION_FOCUS:
        case ACTION_NONE:
            break;
    }
}

void ungrab_pointer(void)
{
    PUTS("ungrab pointer");

    if (frozen_pointer->action == ACTION_NONE)
        return;

    if (is_floating(frozen_pointer->client))
        update_floating_rectangle(frozen_pointer->client);
    monitor_t *m = underlying_monitor(frozen_pointer->client);
    if (m != NULL && m != frozen_pointer->monitor) {
        transfer_node(frozen_pointer->monitor, frozen_pointer->desktop, m, m->desk, frozen_pointer->node);
        select_monitor(m);
    }
}
