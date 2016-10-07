/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Copyright 2008 - 2011 Red Hat, Inc.
 */

#ifndef __NM_OBJECT_PRIVATE_H__
#define __NM_OBJECT_PRIVATE_H__

#include "nm-object.h"

typedef gboolean (*PropertyMarshalFunc) (NMObject *, GParamSpec *, GVariant *, gpointer);

typedef GObject * (*NMObjectCreatorFunc) (GDBusConnection *, const char *);

typedef struct {
	const char *name;
	gpointer field;
	PropertyMarshalFunc func;
	GType object_type;
	const char *signal_prefix;
} NMPropertiesInfo;

void _nm_object_register_properties (NMObject *object,
                                     const char *interface,
                                     const NMPropertiesInfo *info);

void _nm_object_queue_notify (NMObject *object, const char *property);

/* DBus property accessors */

void _nm_object_set_property (NMObject *object,
                              const char *interface,
                              const char *prop_name,
                              const char *format_string,
                              ...);

GDBusProxy *_nm_object_get_proxy (NMObject   *object,
                                  const char *interface);

#endif /* __NM_OBJECT_PRIVATE_H__ */
