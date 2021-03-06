/*
 *  Dynamic device configuration and creation.
 *
 *  Copyright (c) 2009 CodeSourcery
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/* The theory here is that it should be possible to create a machine without
   knowledge of specific devices.  Historically board init routines have
   passed a bunch of arguments to each device, requiring the board know
   exactly which device it is dealing with.  This file provides an abstract
   API for device configuration and initialization.  Devices will generally
   inherit from a particular bus (e.g. PCI or I2C) rather than
   this API directly.  */

#include "hw/qdev.h"
#include "sysemu/sysemu.h"
#include "qapi/error.h"
#include "qapi/qmp/qerror.h"
#include "qapi/visitor.h"
#include "qapi/qmp/qjson.h"
#include "monitor/monitor.h"

int qdev_hotplug = 0;
static bool qdev_hot_added = false;
static bool qdev_hot_removed = false;

const VMStateDescription *qdev_get_vmsd(DeviceState *dev)
{
    DeviceClass *dc = DEVICE_GET_CLASS(dev);
    return dc->vmsd;
}

const char *qdev_fw_name(DeviceState *dev)
{
    DeviceClass *dc = DEVICE_GET_CLASS(dev);

    if (dc->fw_name) {
        return dc->fw_name;
    }

    return object_get_typename(OBJECT(dev));
}

static void qdev_property_add_legacy(DeviceState *dev, Property *prop,
                                     Error **errp);

static void bus_remove_child(BusState *bus, DeviceState *child)
{
    BusChild *kid;

    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        if (kid->child == child) {
            char name[32];

            snprintf(name, sizeof(name), "child[%d]", kid->index);
            QTAILQ_REMOVE(&bus->children, kid, sibling);

            /* This gives back ownership of kid->child back to us.  */
            object_property_del(OBJECT(bus), name, NULL);
            object_unref(OBJECT(kid->child));
            g_free(kid);
            return;
        }
    }
}

static void bus_add_child(BusState *bus, DeviceState *child)
{
    char name[32];
    BusChild *kid = g_malloc0(sizeof(*kid));

    if (qdev_hotplug) {
        assert(bus->allow_hotplug);
    }

    kid->index = bus->max_index++;
    kid->child = child;
    object_ref(OBJECT(kid->child));

    QTAILQ_INSERT_HEAD(&bus->children, kid, sibling);

    /* This transfers ownership of kid->child to the property.  */
    snprintf(name, sizeof(name), "child[%d]", kid->index);
    object_property_add_link(OBJECT(bus), name,
                             object_get_typename(OBJECT(child)),
                             (Object **)&kid->child,
                             NULL);
}

void qdev_set_parent_bus(DeviceState *dev, BusState *bus)
{
    dev->parent_bus = bus;
    object_ref(OBJECT(bus));
    bus_add_child(bus, dev);
}

/* Create a new device.  This only initializes the device state structure
   and allows properties to be set.  qdev_init should be called to
   initialize the actual device emulation.  */
DeviceState *qdev_create(BusState *bus, const char *name)
{
    DeviceState *dev;

    dev = qdev_try_create(bus, name);
    if (!dev) {
        if (bus) {
            error_report("Unknown device '%s' for bus '%s'", name,
                         object_get_typename(OBJECT(bus)));
        } else {
            error_report("Unknown device '%s' for default sysbus", name);
        }
        abort();
    }

    return dev;
}

DeviceState *qdev_try_create(BusState *bus, const char *type)
{
    DeviceState *dev;

    if (object_class_by_name(type) == NULL) {
        return NULL;
    }
    dev = DEVICE(object_new(type));
    if (!dev) {
        return NULL;
    }

    if (!bus) {
        bus = sysbus_get_default();
    }

    qdev_set_parent_bus(dev, bus);
    object_unref(OBJECT(dev));
    return dev;
}

/* Initialize a device.  Device properties should be set before calling
   this function.  IRQs and MMIO regions should be connected/mapped after
   calling this function.
   On failure, destroy the device and return negative value.
   Return 0 on success.  */
int qdev_init(DeviceState *dev)
{
    Error *local_err = NULL;

    assert(!dev->realized);

    object_property_set_bool(OBJECT(dev), true, "realized", &local_err);
    if (local_err != NULL) {
        error_free(local_err);
        qdev_free(dev);
        return -1;
    }
    return 0;
}

static void device_realize(DeviceState *dev, Error **err)
{
    DeviceClass *dc = DEVICE_GET_CLASS(dev);

    if (dc->init) {
        int rc = dc->init(dev);
        if (rc < 0) {
            error_setg(err, "Device initialization failed.");
            return;
        }
    }
}

void qdev_set_legacy_instance_id(DeviceState *dev, int alias_id,
                                 int required_for_version)
{
    assert(!dev->realized);
    dev->instance_id_alias = alias_id;
    dev->alias_required_for_version = required_for_version;
}

void qdev_unplug(DeviceState *dev, Error **errp)
{
    DeviceClass *dc = DEVICE_GET_CLASS(dev);

    if (!dev->parent_bus->allow_hotplug) {
        error_set(errp, QERR_BUS_NO_HOTPLUG, dev->parent_bus->name);
        return;
    }
    assert(dc->unplug != NULL);

    qdev_hot_removed = true;

    if (dc->unplug(dev) < 0) {
        error_set(errp, QERR_UNDEFINED_ERROR);
        return;
    }
}

static int qdev_reset_one(DeviceState *dev, void *opaque)
{
    device_reset(dev);

    return 0;
}

static int qbus_reset_one(BusState *bus, void *opaque)
{
    BusClass *bc = BUS_GET_CLASS(bus);
    if (bc->reset) {
        return bc->reset(bus);
    }
    return 0;
}

void qdev_reset_all(DeviceState *dev)
{
    qdev_walk_children(dev, qdev_reset_one, qbus_reset_one, NULL);
}

void qbus_reset_all(BusState *bus)
{
    qbus_walk_children(bus, qdev_reset_one, qbus_reset_one, NULL);
}

void qbus_reset_all_fn(void *opaque)
{
    BusState *bus = opaque;
    qbus_reset_all(bus);
}

/* can be used as ->unplug() callback for the simple cases */
int qdev_simple_unplug_cb(DeviceState *dev)
{
    /* just zap it */
    qdev_free(dev);
    return 0;
}


/* Like qdev_init(), but terminate program via error_report() instead of
   returning an error value.  This is okay during machine creation.
   Don't use for hotplug, because there callers need to recover from
   failure.  Exception: if you know the device's init() callback can't
   fail, then qdev_init_nofail() can't fail either, and is therefore
   usable even then.  But relying on the device implementation that
   way is somewhat unclean, and best avoided.  */
void qdev_init_nofail(DeviceState *dev)
{
    const char *typename = object_get_typename(OBJECT(dev));

    if (qdev_init(dev) < 0) {
        error_report("Initialization of device %s failed", typename);
        exit(1);
    }
}

/* Unlink device from bus and free the structure.  */
void qdev_free(DeviceState *dev)
{
    object_unparent(OBJECT(dev));
}

void qdev_machine_creation_done(void)
{
    /*
     * ok, initial machine setup is done, starting from now we can
     * only create hotpluggable devices
     */
    qdev_hotplug = 1;
}

bool qdev_machine_modified(void)
{
    return qdev_hot_added || qdev_hot_removed;
}

BusState *qdev_get_parent_bus(DeviceState *dev)
{
    return dev->parent_bus;
}

void qdev_init_gpio_in(DeviceState *dev, qemu_irq_handler handler, int n)
{
    dev->gpio_in = qemu_extend_irqs(dev->gpio_in, dev->num_gpio_in, handler,
                                        dev, n);
    dev->num_gpio_in += n;
}

void qdev_init_gpio_out(DeviceState *dev, qemu_irq *pins, int n)
{
    assert(dev->num_gpio_out == 0);
    dev->num_gpio_out = n;
    dev->gpio_out = pins;
}

qemu_irq qdev_get_gpio_in(DeviceState *dev, int n)
{
    assert(n >= 0 && n < dev->num_gpio_in);
    return dev->gpio_in[n];
}

void qdev_connect_gpio_out(DeviceState * dev, int n, qemu_irq pin)
{
    assert(n >= 0 && n < dev->num_gpio_out);
    dev->gpio_out[n] = pin;
}

BusState *qdev_get_child_bus(DeviceState *dev, const char *name)
{
    BusState *bus;

    QLIST_FOREACH(bus, &dev->child_bus, sibling) {
        if (strcmp(name, bus->name) == 0) {
            return bus;
        }
    }
    return NULL;
}

int qbus_walk_children(BusState *bus, qdev_walkerfn *devfn,
                       qbus_walkerfn *busfn, void *opaque)
{
    BusChild *kid;
    int err;

    if (busfn) {
        err = busfn(bus, opaque);
        if (err) {
            return err;
        }
    }

    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        err = qdev_walk_children(kid->child, devfn, busfn, opaque);
        if (err < 0) {
            return err;
        }
    }

    return 0;
}

int qdev_walk_children(DeviceState *dev, qdev_walkerfn *devfn,
                       qbus_walkerfn *busfn, void *opaque)
{
    BusState *bus;
    int err;

    if (devfn) {
        err = devfn(dev, opaque);
        if (err) {
            return err;
        }
    }

    QLIST_FOREACH(bus, &dev->child_bus, sibling) {
        err = qbus_walk_children(bus, devfn, busfn, opaque);
        if (err < 0) {
            return err;
        }
    }

    return 0;
}

DeviceState *qdev_find_recursive(BusState *bus, const char *id)
{
    BusChild *kid;
    DeviceState *ret;
    BusState *child;

    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        DeviceState *dev = kid->child;

        if (dev->id && strcmp(dev->id, id) == 0) {
            return dev;
        }

        QLIST_FOREACH(child, &dev->child_bus, sibling) {
            ret = qdev_find_recursive(child, id);
            if (ret) {
                return ret;
            }
        }
    }
    return NULL;
}

static void qbus_realize(BusState *bus, DeviceState *parent, const char *name)
{
    const char *typename = object_get_typename(OBJECT(bus));
    char *buf;
    int i,len;

    bus->parent = parent;

    if (name) {
        bus->name = g_strdup(name);
    } else if (bus->parent && bus->parent->id) {
        /* parent device has id -> use it for bus name */
        len = strlen(bus->parent->id) + 16;
        buf = g_malloc(len);
        snprintf(buf, len, "%s.%d", bus->parent->id, bus->parent->num_child_bus);
        bus->name = buf;
    } else {
        /* no id -> use lowercase bus type for bus name */
        len = strlen(typename) + 16;
        buf = g_malloc(len);
        len = snprintf(buf, len, "%s.%d", typename,
                       bus->parent ? bus->parent->num_child_bus : 0);
        for (i = 0; i < len; i++)
            buf[i] = qemu_tolower(buf[i]);
        bus->name = buf;
    }

    if (bus->parent) {
        QLIST_INSERT_HEAD(&bus->parent->child_bus, bus, sibling);
        bus->parent->num_child_bus++;
        object_property_add_child(OBJECT(bus->parent), bus->name, OBJECT(bus), NULL);
        object_unref(OBJECT(bus));
    } else if (bus != sysbus_get_default()) {
        /* TODO: once all bus devices are qdevified,
           only reset handler for main_system_bus should be registered here. */
        qemu_register_reset(qbus_reset_all_fn, bus);
    }
}

static void bus_unparent(Object *obj)
{
    BusState *bus = BUS(obj);
    BusChild *kid;

    while ((kid = QTAILQ_FIRST(&bus->children)) != NULL) {
        DeviceState *dev = kid->child;
        qdev_free(dev);
    }
    if (bus->parent) {
        QLIST_REMOVE(bus, sibling);
        bus->parent->num_child_bus--;
        bus->parent = NULL;
    } else {
        assert(bus != sysbus_get_default()); /* main_system_bus is never freed */
        qemu_unregister_reset(qbus_reset_all_fn, bus);
    }
}

void qbus_create_inplace(void *bus, const char *typename,
                         DeviceState *parent, const char *name)
{
    object_initialize(bus, typename);
    qbus_realize(bus, parent, name);
}

BusState *qbus_create(const char *typename, DeviceState *parent, const char *name)
{
    BusState *bus;

    bus = BUS(object_new(typename));
    qbus_realize(bus, parent, name);

    return bus;
}

void qbus_free(BusState *bus)
{
    object_unparent(OBJECT(bus));
}

static char *bus_get_fw_dev_path(BusState *bus, DeviceState *dev)
{
    BusClass *bc = BUS_GET_CLASS(bus);

    if (bc->get_fw_dev_path) {
        return bc->get_fw_dev_path(dev);
    }

    return NULL;
}

static int qdev_get_fw_dev_path_helper(DeviceState *dev, char *p, int size)
{
    int l = 0;

    if (dev && dev->parent_bus) {
        char *d;
        l = qdev_get_fw_dev_path_helper(dev->parent_bus->parent, p, size);
        d = bus_get_fw_dev_path(dev->parent_bus, dev);
        if (d) {
            l += snprintf(p + l, size - l, "%s", d);
            g_free(d);
        } else {
            l += snprintf(p + l, size - l, "%s", object_get_typename(OBJECT(dev)));
        }
    }
    l += snprintf(p + l , size - l, "/");

    return l;
}

char* qdev_get_fw_dev_path(DeviceState *dev)
{
    char path[128];
    int l;

    l = qdev_get_fw_dev_path_helper(dev, path, 128);

    path[l-1] = '\0';

    return g_strdup(path);
}

char *qdev_get_dev_path(DeviceState *dev)
{
    BusClass *bc;

    if (!dev || !dev->parent_bus) {
        return NULL;
    }

    bc = BUS_GET_CLASS(dev->parent_bus);
    if (bc->get_dev_path) {
        return bc->get_dev_path(dev);
    }

    return NULL;
}

/**
 * Legacy property handling
 */

static void qdev_get_legacy_property(Object *obj, Visitor *v, void *opaque,
                                     const char *name, Error **errp)
{
    DeviceState *dev = DEVICE(obj);
    Property *prop = opaque;

    char buffer[1024];
    char *ptr = buffer;

    prop->info->print(dev, prop, buffer, sizeof(buffer));
    visit_type_str(v, &ptr, name, errp);
}

static void qdev_set_legacy_property(Object *obj, Visitor *v, void *opaque,
                                     const char *name, Error **errp)
{
    DeviceState *dev = DEVICE(obj);
    Property *prop = opaque;
    Error *local_err = NULL;
    char *ptr = NULL;
    int ret;

    if (dev->realized) {
        qdev_prop_set_after_realize(dev, name, errp);
        return;
    }

    visit_type_str(v, &ptr, name, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    ret = prop->info->parse(dev, prop, ptr);
    error_set_from_qdev_prop_error(errp, ret, dev, prop, ptr);
    g_free(ptr);
}

/**
 * @qdev_add_legacy_property - adds a legacy property
 *
 * Do not use this is new code!  Properties added through this interface will
 * be given names and types in the "legacy" namespace.
 *
 * Legacy properties are string versions of other OOM properties.  The format
 * of the string depends on the property type.
 */
void qdev_property_add_legacy(DeviceState *dev, Property *prop,
                              Error **errp)
{
    gchar *name, *type;

    /* Register pointer properties as legacy properties */
    if (!prop->info->print && !prop->info->parse &&
        (prop->info->set || prop->info->get)) {
        return;
    }

    name = g_strdup_printf("legacy-%s", prop->name);
    type = g_strdup_printf("legacy<%s>",
                           prop->info->legacy_name ?: prop->info->name);

    object_property_add(OBJECT(dev), name, type,
                        prop->info->print ? qdev_get_legacy_property : prop->info->get,
                        prop->info->parse ? qdev_set_legacy_property : prop->info->set,
                        NULL,
                        prop, errp);

    g_free(type);
    g_free(name);
}

/**
 * @qdev_property_add_static - add a @Property to a device.
 *
 * Static properties access data in a struct.  The actual type of the
 * property and the field depends on the property type.
 */
void qdev_property_add_static(DeviceState *dev, Property *prop,
                              Error **errp)
{
    Error *local_err = NULL;
    Object *obj = OBJECT(dev);

    /*
     * TODO qdev_prop_ptr does not have getters or setters.  It must
     * go now that it can be replaced with links.  The test should be
     * removed along with it: all static properties are read/write.
     */
    if (!prop->info->get && !prop->info->set) {
        return;
    }

    object_property_add(obj, prop->name, prop->info->name,
                        prop->info->get, prop->info->set,
                        prop->info->release,
                        prop, &local_err);

    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    if (prop->qtype == QTYPE_NONE) {
        return;
    }

    if (prop->qtype == QTYPE_QBOOL) {
        object_property_set_bool(obj, prop->defval, prop->name, &local_err);
    } else if (prop->info->enum_table) {
        object_property_set_str(obj, prop->info->enum_table[prop->defval],
                                prop->name, &local_err);
    } else if (prop->qtype == QTYPE_QINT) {
        object_property_set_int(obj, prop->defval, prop->name, &local_err);
    }
    assert_no_error(local_err);
}

static bool device_get_realized(Object *obj, Error **err)
{
    DeviceState *dev = DEVICE(obj);
    return dev->realized;
}

static void device_set_realized(Object *obj, bool value, Error **err)
{
    DeviceState *dev = DEVICE(obj);
    DeviceClass *dc = DEVICE_GET_CLASS(dev);
    Error *local_err = NULL;

    if (value && !dev->realized) {
        if (dc->realize) {
            dc->realize(dev, &local_err);
        }

        if (!obj->parent && local_err == NULL) {
            static int unattached_count;
            gchar *name = g_strdup_printf("device[%d]", unattached_count++);

            object_property_add_child(container_get(qdev_get_machine(),
                                                    "/unattached"),
                                      name, obj, &local_err);
            g_free(name);
        }

        if (qdev_get_vmsd(dev) && local_err == NULL) {
            vmstate_register_with_alias_id(dev, -1, qdev_get_vmsd(dev), dev,
                                           dev->instance_id_alias,
                                           dev->alias_required_for_version);
        }
        if (dev->hotplugged && local_err == NULL) {
            device_reset(dev);
        }
    } else if (!value && dev->realized) {
        if (dc->unrealize) {
            dc->unrealize(dev, &local_err);
        }
    }

    if (local_err != NULL) {
        error_propagate(err, local_err);
        return;
    }

    dev->realized = value;
}

static void device_initfn(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    ObjectClass *class;
    Property *prop;
    Error *err = NULL;

    if (qdev_hotplug) {
        dev->hotplugged = 1;
        qdev_hot_added = true;
    }

    dev->instance_id_alias = -1;
    dev->realized = false;

    object_property_add_bool(obj, "realized",
                             device_get_realized, device_set_realized, NULL);

    class = object_get_class(OBJECT(dev));
    do {
        for (prop = DEVICE_CLASS(class)->props; prop && prop->name; prop++) {
            qdev_property_add_legacy(dev, prop, &err);
            assert_no_error(err);
            qdev_property_add_static(dev, prop, &err);
            assert_no_error(err);
        }
        class = object_class_get_parent(class);
    } while (class != object_class_by_name(TYPE_DEVICE));
    qdev_prop_set_globals(dev);

    object_property_add_link(OBJECT(dev), "parent_bus", TYPE_BUS,
                             (Object **)&dev->parent_bus, &err);
    assert_no_error(err);
}

/* Unlink device from bus and free the structure.  */
static void device_finalize(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    if (dev->opts) {
        qemu_opts_del(dev->opts);
    }
}

static void device_class_base_init(ObjectClass *class, void *data)
{
    DeviceClass *klass = DEVICE_CLASS(class);

    /* We explicitly look up properties in the superclasses,
     * so do not propagate them to the subclasses.
     */
    klass->props = NULL;
}

static void device_unparent(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    DeviceClass *dc = DEVICE_GET_CLASS(dev);
    BusState *bus;
    QObject *event_data;
    bool have_realized = dev->realized;

    while (dev->num_child_bus) {
        bus = QLIST_FIRST(&dev->child_bus);
        qbus_free(bus);
    }
    if (dev->realized) {
        if (qdev_get_vmsd(dev)) {
            vmstate_unregister(dev, qdev_get_vmsd(dev), dev);
        }
        if (dc->exit) {
            dc->exit(dev);
        }
    }
    if (dev->parent_bus) {
        bus_remove_child(dev->parent_bus, dev);
        object_unref(OBJECT(dev->parent_bus));
        dev->parent_bus = NULL;
    }

    /* Only send event if the device had been completely realized */
    if (have_realized) {
        gchar *path = object_get_canonical_path(OBJECT(dev));

        if (dev->id) {
            event_data = qobject_from_jsonf("{ 'device': %s, 'path': %s }",
                                            dev->id, path);
        } else {
            event_data = qobject_from_jsonf("{ 'path': %s }", path);
        }
        monitor_protocol_event(QEVENT_DEVICE_DELETED, event_data);
        qobject_decref(event_data);
        g_free(path);
    }
}

static void device_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);

    class->unparent = device_unparent;
    dc->realize = device_realize;
}

void device_reset(DeviceState *dev)
{
    DeviceClass *klass = DEVICE_GET_CLASS(dev);

    if (klass->reset) {
        klass->reset(dev);
    }
}

Object *qdev_get_machine(void)
{
    static Object *dev;

    if (dev == NULL) {
        dev = container_get(object_get_root(), "/machine");
    }

    return dev;
}

static const TypeInfo device_type_info = {
    .name = TYPE_DEVICE,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(DeviceState),
    .instance_init = device_initfn,
    .instance_finalize = device_finalize,
    .class_base_init = device_class_base_init,
    .class_init = device_class_init,
    .abstract = true,
    .class_size = sizeof(DeviceClass),
};

static void qbus_initfn(Object *obj)
{
    BusState *bus = BUS(obj);

    QTAILQ_INIT(&bus->children);
}

static void bus_class_init(ObjectClass *class, void *data)
{
    class->unparent = bus_unparent;
}

static void qbus_finalize(Object *obj)
{
    BusState *bus = BUS(obj);

    g_free((char *)bus->name);
}

static const TypeInfo bus_info = {
    .name = TYPE_BUS,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(BusState),
    .abstract = true,
    .class_size = sizeof(BusClass),
    .instance_init = qbus_initfn,
    .instance_finalize = qbus_finalize,
    .class_init = bus_class_init,
};

static void qdev_register_types(void)
{
    type_register_static(&bus_info);
    type_register_static(&device_type_info);
}

type_init(qdev_register_types)
