
#include <cmath>
#include <stdlib.h>
#include <SDL2/SDL_platform.h>
#include <SDL2/SDL_opengl.h>

#include "core/gl_util.h"
#include "core/console.h"
#include "core/vmath.h"
#include "core/polygon.h"
#include "core/obb.h"
#include "render.h"
#include "world.h"
#include "portal.h"
#include "frustum.h"
#include "camera.h"
#include "script.h"
#include "mesh.h"
#include "hair.h"
#include "entity.h"
#include "engine.h"
#include "engine_bullet.h"
#include "bsp_tree.h"
#include "resource.h"
#include "shader_description.h"
#include "shader_manager.h"

render_t renderer;
class dynamicBSP render_dBSP(512 * 1024);

static uint16_t active_transparency = 0;
static GLuint   active_texture = 0;

/*GLhandleARB main_vsh, main_fsh, main_program;
GLint       main_model_mat_pos, main_proj_mat_pos, main_model_proj_mat_pos, main_tr_mat_pos;
*/
/*bool btCollisionObjectIsVisible(btCollisionObject *colObj)
{
    engine_container_p cont = (engine_container_p)colObj->getUserPointer();
    return (cont == NULL) || (cont->room == NULL) || (cont->room->is_in_r_list && cont->room->active);
}*/

void Render_InitGlobals()
{
    renderer.settings.anisotropy = 0;
    renderer.settings.lod_bias = 0;
    renderer.settings.antialias = 0;
    renderer.settings.antialias_samples = 0;
    renderer.settings.mipmaps = 3;
    renderer.settings.mipmap_mode = 3;
    renderer.settings.texture_border = 8;
    renderer.settings.z_depth = 16;
    renderer.settings.fog_enabled = 1;
    renderer.settings.fog_color[0] = 0.0f;
    renderer.settings.fog_color[1] = 0.0f;
    renderer.settings.fog_color[2] = 0.0f;
    renderer.settings.fog_start_depth = 10000.0f;
    renderer.settings.fog_end_depth = 16000.0f;
}

void Render_DoShaders()
{
    renderer.shader_manager = new shader_manager();
}


void Render_Init()
{
    renderer.blocked = 1;
    renderer.cam = NULL;

    renderer.r_list = NULL;
    renderer.r_list_size = 0;
    renderer.r_list_active_count= 0;

    renderer.world = NULL;
    renderer.style = 0x00;
}


void Render_Empty(render_p render)
{
    render->world = NULL;

    if(render->r_list)
    {
        render->r_list_active_count = 0;
        render->r_list_size = 0;
        free(render->r_list);
        render->r_list = NULL;
    }

    if (render->shader_manager)
    {
        delete render->shader_manager;
        render->shader_manager = 0;
    }
}


render_list_p Render_CreateRoomListArray(unsigned int count)
{
    render_list_p ret = (render_list_p)malloc(count * sizeof(render_list_t));

    for(unsigned int i=0; i<count; i++)
    {
        ret[i].active = 0;
        ret[i].room = NULL;
        ret[i].dist = 0.0;
    }
    return ret;
}

void Render_SkyBox(const btScalar modelViewProjectionMatrix[16])
{
    btScalar tr[16];
    btScalar *p;

    if((renderer.style & R_DRAW_SKYBOX) && (renderer.world != NULL) && (renderer.world->sky_box != NULL))
    {
        glDepthMask(GL_FALSE);
        tr[15] = 1.0;
        p = renderer.world->sky_box->animations->frames->bone_tags->offset;
        vec3_add(tr+12, renderer.cam->pos, p);
        p = renderer.world->sky_box->animations->frames->bone_tags->qrotate;
        Mat4_set_qrotation(tr, p);
        btScalar fullView[16];
        Mat4_Mat4_mul(fullView, modelViewProjectionMatrix, tr);

        const unlit_tinted_shader_description *shader = renderer.shader_manager->getStaticMeshShader();
        glUseProgramObjectARB(shader->program);
        glUniformMatrix4fvARB(shader->model_view_projection, 1, false, fullView);
        glUniform1iARB(shader->sampler, 0);
        GLfloat tint[] = { 1, 1, 1, 1 };
        glUniform4fvARB(shader->tint_mult, 1, tint);

        Render_Mesh(renderer.world->sky_box->mesh_tree->mesh_base, NULL, NULL);
        glDepthMask(GL_TRUE);
    }
}

/**
 * Opaque meshes drawing
 */
void Render_Mesh(struct base_mesh_s *mesh, const btScalar *overrideVertices, const btScalar *overrideNormals)
{
    if(mesh->num_animated_elements > 0)
    {
        // Respecify the tex coord buffer
        glBindBufferARB(GL_ARRAY_BUFFER, mesh->animated_texcoord_array);
        // Tell OpenGL to discard the old values
        glBufferDataARB(GL_ARRAY_BUFFER, mesh->num_animated_elements * sizeof(GLfloat [2]), 0, GL_STREAM_DRAW);
        // Get writable data (to avoid copy)
        GLfloat *data = (GLfloat *) glMapBufferARB(GL_ARRAY_BUFFER, GL_WRITE_ONLY);

        size_t offset = 0;
        for(polygon_p p=mesh->animated_polygons;p!=NULL;p=p->next)
        {
            anim_seq_p seq = engine_world.anim_sequences + p->anim_id - 1;
            uint16_t frame = (seq->current_frame + p->frame_offset) % seq->frames_count;
            tex_frame_p tf = seq->frames + frame;
            for(uint16_t i=0;i<p->vertex_count;i++)
            {
                const GLfloat *v = p->vertices[i].tex_coord;
                data[offset + 0] = tf->mat[0+0*2] * v[0] + tf->mat[0+1*2] * v[1] + tf->move[0];
                data[offset + 1] = tf->mat[1+0*2] * v[0] + tf->mat[1+1*2] * v[1] + tf->move[1] - tf->current_uvrotate;

                offset += 2;
            }
        }
        glUnmapBufferARB(GL_ARRAY_BUFFER);

        // Setup altered buffer
        glTexCoordPointer(2, GL_FLOAT, sizeof(GLfloat [2]), 0);
        // Setup static data
        glBindBufferARB(GL_ARRAY_BUFFER, mesh->animated_vertex_array);
        glVertexPointer(3, GL_BT_SCALAR, sizeof(GLfloat [10]), 0);
        glColorPointer(4, GL_FLOAT, sizeof(GLfloat [10]), (void *) sizeof(GLfloat [3]));
        glNormalPointer(GL_FLOAT, sizeof(GLfloat [10]), (void *) sizeof(GLfloat [7]));

        glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER, mesh->animated_index_array);
        if(active_texture != renderer.world->textures[0])
        {
            active_texture = renderer.world->textures[0];
            glBindTexture(GL_TEXTURE_2D, active_texture);
        }
        glDrawElements(GL_TRIANGLES, mesh->animated_index_array_length, GL_UNSIGNED_INT, 0);
    }

    if(mesh->vertex_count == 0)
    {
        return;
    }

    if(mesh->vbo_vertex_array)
    {
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, mesh->vbo_vertex_array);
        glVertexPointer(3, GL_BT_SCALAR, sizeof(vertex_t), (void*)offsetof(vertex_t, position));
        glColorPointer(4, GL_FLOAT, sizeof(vertex_t), (void*)offsetof(vertex_t, color));
        glNormalPointer(GL_FLOAT, sizeof(vertex_t), (void*)offsetof(vertex_t, normal));
        glTexCoordPointer(2, GL_FLOAT, sizeof(vertex_t), (void*)offsetof(vertex_t, tex_coord));
    }

    // Bind overriden vertices if they exist
    if (overrideVertices != NULL)
    {
        // Standard normals are always float. Overridden normals (from skinning)
        // are btScalar.
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
        glVertexPointer(3, GL_BT_SCALAR, 0, overrideVertices);
        glNormalPointer(GL_BT_SCALAR, 0, overrideNormals);
    }

    const uint32_t *elementsbase = mesh->elements;
        glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, mesh->vbo_index_array);
        elementsbase = NULL;

    unsigned long offset = 0;
    for(uint32_t texture = 0; texture < mesh->num_texture_pages; texture++)
    {
        if(mesh->element_count_per_texture[texture] == 0)
        {
            continue;
        }

        if(active_texture != renderer.world->textures[texture])
        {
            active_texture = renderer.world->textures[texture];
            glBindTexture(GL_TEXTURE_2D, active_texture);
        }
        glDrawElements(GL_TRIANGLES, mesh->element_count_per_texture[texture], GL_UNSIGNED_INT, elementsbase + offset);
        offset += mesh->element_count_per_texture[texture];
    }
}


/**
 * draw transparency polygons
 */
void Render_BSPPolygon(struct bsp_polygon_s *p)
{
    // Blending mode switcher.
    // Note that modes above 2 aren't explicitly used in TR textures, only for
    // internal particle processing. Theoretically it's still possible to use
    // them if you will force type via TRTextur utility.
    if(active_transparency != p->transparency)
    {
        active_transparency = p->transparency;
        switch(active_transparency)
        {
            case BM_MULTIPLY:                                    // Classic PC alpha
                glBlendFunc(GL_ONE, GL_ONE);
                break;

            case BM_INVERT_SRC:                                  // Inversion by src (PS darkness) - SAME AS IN TR3-TR5
                glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
                break;

            case BM_INVERT_DEST:                                 // Inversion by dest
                glBlendFunc(GL_ONE_MINUS_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR);
                break;

            case BM_SCREEN:                                      // Screen (smoke, etc.)
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
                break;

            case BM_ANIMATED_TEX:
                glBlendFunc(GL_ONE, GL_ZERO);
                break;

            default:                                             // opaque animated textures case
                break;
        };
    }

    if(active_texture != renderer.world->textures[p->tex_index])
    {
        active_texture = renderer.world->textures[p->tex_index];
        glBindTexture(GL_TEXTURE_2D, active_texture);
    }
    glDrawElements(GL_TRIANGLE_FAN, p->vertex_count, GL_UNSIGNED_INT, p->indexes);
}


void Render_BSPFrontToBack(struct bsp_node_s *root)
{
    btScalar d = vec3_plane_dist(root->plane, engine_camera.pos);

    if(d >= 0)
    {
        if(root->front != NULL)
        {
            Render_BSPFrontToBack(root->front);
        }

        for(bsp_polygon_p p=root->polygons_front;p!=NULL;p=p->next)
        {
            Render_BSPPolygon(p);
        }
        for(bsp_polygon_p p=root->polygons_back;p!=NULL;p=p->next)
        {
            Render_BSPPolygon(p);
        }

        if(root->back != NULL)
        {
            Render_BSPFrontToBack(root->back);
        }
    }
    else
    {
        if(root->back != NULL)
        {
            Render_BSPFrontToBack(root->back);
        }

        for(bsp_polygon_p p=root->polygons_back;p!=NULL;p=p->next)
        {
            Render_BSPPolygon(p);
        }
        for(bsp_polygon_p p=root->polygons_front;p!=NULL;p=p->next)
        {
            Render_BSPPolygon(p);
        }

        if(root->front != NULL)
        {
            Render_BSPFrontToBack(root->front);
        }
    }
}

void Render_BSPBackToFront(struct bsp_node_s *root)
{
    btScalar d = vec3_plane_dist(root->plane, engine_camera.pos);

    if(d >= 0)
    {
        if(root->back != NULL)
        {
            Render_BSPBackToFront(root->back);
        }

        for(bsp_polygon_p p=root->polygons_back;p!=NULL;p=p->next)
        {
            Render_BSPPolygon(p);
        }
        for(bsp_polygon_p p=root->polygons_front;p!=NULL;p=p->next)
        {
            Render_BSPPolygon(p);
        }

        if(root->front != NULL)
        {
            Render_BSPBackToFront(root->front);
        }
    }
    else
    {
        if(root->front != NULL)
        {
            Render_BSPBackToFront(root->front);
        }

        for(bsp_polygon_p p=root->polygons_front;p!=NULL;p=p->next)
        {
            Render_BSPPolygon(p);
        }
        for(bsp_polygon_p p=root->polygons_back;p!=NULL;p=p->next)
        {
            Render_BSPPolygon(p);
        }

        if(root->back != NULL)
        {
            Render_BSPBackToFront(root->back);
        }
    }
}

void Render_UpdateAnimTextures()                                                // This function is used for updating global animated texture frame
{
    anim_seq_p seq = engine_world.anim_sequences;
    for(uint16_t i=0;i<engine_world.anim_sequences_count;i++,seq++)
    {
        if(seq->frame_lock)
        {
            continue;
        }

        seq->frame_time += engine_frame_time;
        if(seq->uvrotate)
        {
            int j = (seq->frame_time / seq->frame_rate);
            seq->frame_time -= (btScalar)j * seq->frame_rate;
            seq->frames[seq->current_frame].current_uvrotate = seq->frame_time * seq->frames[seq->current_frame].uvrotate_max / seq->frame_rate;
        }
        else if(seq->frame_time >= seq->frame_rate)
        {
            int j = (seq->frame_time / seq->frame_rate);
            seq->frame_time -= (btScalar)j * seq->frame_rate;

            switch(seq->anim_type)
            {
                case TR_ANIMTEXTURE_REVERSE:
                    if(seq->reverse_direction)
                    {
                        if(seq->current_frame == 0)
                        {
                            seq->current_frame++;
                            seq->reverse_direction = false;
                        }
                        else if(seq->current_frame > 0)
                        {
                            seq->current_frame--;
                        }
                    }
                    else
                    {
                        if(seq->current_frame == seq->frames_count - 1)
                        {
                            seq->current_frame--;
                            seq->reverse_direction = true;
                        }
                        else if(seq->current_frame < seq->frames_count - 1)
                        {
                            seq->current_frame++;
                        }
                        seq->current_frame %= seq->frames_count;                ///@PARANOID
                    }
                    break;

                case TR_ANIMTEXTURE_FORWARD:                                    // inversed in polygon anim. texture frames
                case TR_ANIMTEXTURE_BACKWARD:
                    seq->current_frame++;
                    seq->current_frame %= seq->frames_count;
                    break;
            };
        }
    }
}


void Render_SkinMesh(struct base_mesh_s *mesh, btScalar transform[16])
{
    uint32_t i;
    vertex_p v;
    btScalar *p_vertex, *src_v, *dst_v, t;
    GLfloat *p_normale, *src_n, *dst_n;
    int8_t *ch = mesh->skin_map;
    size_t buf_size = mesh->vertex_count * 3 * sizeof(GLfloat);

    p_vertex  = (GLfloat*)Sys_GetTempMem(buf_size);
    p_normale = (GLfloat*)Sys_GetTempMem(buf_size);
    dst_v = p_vertex;
    dst_n = p_normale;
    v = mesh->vertices;
    for(i=0; i<mesh->vertex_count; i++,v++)
    {
        src_v = v->position;
        src_n = v->normal;
        switch(*ch)
        {
        case 0:
            dst_v[0]  = transform[0] * src_v[0] + transform[1] * src_v[1] + transform[2]  * src_v[2];             // (M^-1 * src).x
            dst_v[1]  = transform[4] * src_v[0] + transform[5] * src_v[1] + transform[6]  * src_v[2];             // (M^-1 * src).y
            dst_v[2]  = transform[8] * src_v[0] + transform[9] * src_v[1] + transform[10] * src_v[2];             // (M^-1 * src).z

            dst_n[0]  = transform[0] * src_n[0] + transform[1] * src_n[1] + transform[2]  * src_n[2];             // (M^-1 * src).x
            dst_n[1]  = transform[4] * src_n[0] + transform[5] * src_n[1] + transform[6]  * src_n[2];             // (M^-1 * src).y
            dst_n[2]  = transform[8] * src_n[0] + transform[9] * src_n[1] + transform[10] * src_n[2];             // (M^-1 * src).z

            vec3_add(dst_v, dst_v, src_v);
            dst_v[0] /= 2.0;
            dst_v[1] /= 2.0;
            dst_v[2] /= 2.0;
            vec3_add(dst_n, dst_n, src_n);
            vec3_norm(dst_n, t);
            break;

        case 2:
            dst_v[0]  = transform[0] * src_v[0] + transform[1] * src_v[1] + transform[2]  * src_v[2];             // (M^-1 * src).x
            dst_v[1]  = transform[4] * src_v[0] + transform[5] * src_v[1] + transform[6]  * src_v[2];             // (M^-1 * src).y
            dst_v[2]  = transform[8] * src_v[0] + transform[9] * src_v[1] + transform[10] * src_v[2];             // (M^-1 * src).z

            dst_n[0]  = transform[0] * src_n[0] + transform[1] * src_n[1] + transform[2]  * src_n[2];             // (M^-1 * src).x
            dst_n[1]  = transform[4] * src_n[0] + transform[5] * src_n[1] + transform[6]  * src_n[2];             // (M^-1 * src).y
            dst_n[2]  = transform[8] * src_n[0] + transform[9] * src_n[1] + transform[10] * src_n[2];             // (M^-1 * src).z
            //vec3_copy(dst_n, src_n);
            break;

        case 1:
            vec3_copy(dst_v, src_v);
            vec3_copy(dst_n, src_n);
            break;
        }
        ch++;
        dst_v += 3;
        dst_n += 3;
    }

    Render_Mesh(mesh, p_vertex, p_normale);
    Sys_ReturnTempMem(buf_size);
}

/**
 * skeletal model drawing
 */
void Render_SkeletalModel(const lit_shader_description *shader, struct ss_bone_frame_s *bframe, const btScalar mvMatrix[16], const btScalar mvpMatrix[16])
{
    ss_bone_tag_p btag = bframe->bone_tags;

    //mvMatrix = modelViewMatrix x entity->transform
    //mvpMatrix = modelViewProjectionMatrix x entity->transform

    for(uint16_t i=0; i<bframe->bone_tag_count; i++,btag++)
    {
        btScalar mvTransform[16];
        Mat4_Mat4_mul(mvTransform, mvMatrix, btag->full_transform);
        glUniformMatrix4fvARB(shader->model_view, 1, false, mvTransform);

        btScalar mvpTransform[16];
        Mat4_Mat4_mul(mvpTransform, mvpMatrix, btag->full_transform);
        glUniformMatrix4fvARB(shader->model_view_projection, 1, false, mvpTransform);

        Render_Mesh(btag->mesh_base, NULL, NULL);
        if(btag->mesh_slot)
        {
            Render_Mesh(btag->mesh_slot, NULL, NULL);
        }
        if(btag->mesh_skin)
        {
            Render_SkinMesh(btag->mesh_skin, btag->transform);
        }
    }
}

/**
 * Sets up the light calculations for the given entity based on its current
 * room. Returns the used shader, which will have been made current already.
 */
const lit_shader_description *render_setupEntityLight(struct entity_s *entity, const btScalar modelViewMatrix[16])
{
    // Calculate lighting
    const lit_shader_description *shader;

    room_s *room = entity->self->room;
    if(room != NULL)
    {
        GLfloat ambient_component[4];

        ambient_component[0] = room->ambient_lighting[0];
        ambient_component[1] = room->ambient_lighting[1];
        ambient_component[2] = room->ambient_lighting[2];
        ambient_component[3] = 1.0f;

        if(room->flags & TR_ROOM_FLAG_WATER)
        {
            Render_CalculateWaterTint(ambient_component, 0);
        }

        GLenum current_light_number = 0;
        light_s *current_light = NULL;

        GLfloat positions[3*MAX_NUM_LIGHTS];
        GLfloat colors[4*MAX_NUM_LIGHTS];
        GLfloat innerRadiuses[1*MAX_NUM_LIGHTS];
        GLfloat outerRadiuses[1*MAX_NUM_LIGHTS];
        memset(positions, 0, sizeof(positions));
        memset(colors, 0, sizeof(colors));
        memset(innerRadiuses, 0, sizeof(innerRadiuses));
        memset(outerRadiuses, 0, sizeof(outerRadiuses));

        for(uint32_t i = 0; i < room->light_count && current_light_number < MAX_NUM_LIGHTS; i++)
        {
            current_light = &room->lights[i];

            float x = entity->transform[12] - current_light->pos[0];
            float y = entity->transform[13] - current_light->pos[1];
            float z = entity->transform[14] - current_light->pos[2];

            float distance = sqrt(x * x + y * y + z * z);

            // Find color
            colors[current_light_number*4 + 0] = std::fmin(std::fmax(current_light->colour[0], 0.0), 1.0);
            colors[current_light_number*4 + 1] = std::fmin(std::fmax(current_light->colour[1], 0.0), 1.0);
            colors[current_light_number*4 + 2] = std::fmin(std::fmax(current_light->colour[2], 0.0), 1.0);
            colors[current_light_number*4 + 3] = std::fmin(std::fmax(current_light->colour[3], 0.0), 1.0);

            if(room->flags & TR_ROOM_FLAG_WATER)
            {
                Render_CalculateWaterTint(colors + current_light_number * 4, 0);
            }

            // Find position
            Mat4_vec3_mul(&positions[3*current_light_number], modelViewMatrix, current_light->pos);

            // Find fall-off
            if(current_light->light_type == LT_SUN)
            {
                innerRadiuses[current_light_number] = 1e20f;
                outerRadiuses[current_light_number] = 1e21f;
                current_light_number++;
            }
            else if(distance <= current_light->outer + 1024.0f && (current_light->light_type == LT_POINT || current_light->light_type == LT_SHADOW))
            {
                innerRadiuses[current_light_number] = std::fabs(current_light->inner);
                outerRadiuses[current_light_number] = std::fabs(current_light->outer);
                current_light_number++;
            }
        }

        shader = renderer.shader_manager->getEntityShader(current_light_number);
        glUseProgramObjectARB(shader->program);
        glUniform4fvARB(shader->light_ambient, 1, ambient_component);
        glUniform4fvARB(shader->light_color, current_light_number, colors);
        glUniform3fvARB(shader->light_position, current_light_number, positions);
        glUniform1fvARB(shader->light_inner_radius, current_light_number, innerRadiuses);
        glUniform1fvARB(shader->light_outer_radius, current_light_number, outerRadiuses);
    } else {
        shader = renderer.shader_manager->getEntityShader(0);
        glUseProgramObjectARB(shader->program);
    }
    return shader;
}

void Render_Entity(struct entity_s *entity, const btScalar modelViewMatrix[16], const btScalar modelViewProjectionMatrix[16])
{
    if(entity->was_rendered || !(entity->state_flags & ENTITY_STATE_VISIBLE) || (entity->bf.animations.model->hide && !(renderer.style & R_DRAW_NULLMESHES)))
    {
        return;
    }

    // Calculate lighting
    const lit_shader_description *shader = render_setupEntityLight(entity, modelViewMatrix);

    if(entity->bf.animations.model && entity->bf.animations.model->animations)
    {
        // base frame offset
        if(entity->type_flags & ENTITY_TYPE_DYNAMIC)
        {
            Render_DynamicEntity(shader, entity, modelViewMatrix, modelViewProjectionMatrix);
        }
        else
        {
            btScalar subModelView[16];
            btScalar subModelViewProjection[16];
            Mat4_Mat4_mul(subModelView, modelViewMatrix, entity->transform);
            Mat4_Mat4_mul(subModelViewProjection, modelViewProjectionMatrix, entity->transform);
            Render_SkeletalModel(shader, &entity->bf, subModelView, subModelViewProjection);
        }
    }
}

void Render_DynamicEntity(const lit_shader_description *shader, struct entity_s *entity, const btScalar modelViewMatrix[16], const btScalar modelViewProjectionMatrix[16])
{
    ss_bone_tag_p btag = entity->bf.bone_tags;

    for(uint16_t i=0; i<entity->bf.bone_tag_count; i++,btag++)
    {
        btScalar mvTransform[16], tr[16];

        entity->bt.bt_body[i]->getWorldTransform().getOpenGLMatrix(tr);
        Mat4_Mat4_mul(mvTransform, modelViewMatrix, tr);
        glUniformMatrix4fvARB(shader->model_view, 1, false, mvTransform);

        btScalar mvpTransform[16];
        Mat4_Mat4_mul(mvpTransform, modelViewProjectionMatrix, tr);
        glUniformMatrix4fvARB(shader->model_view_projection, 1, false, mvpTransform);

        Render_Mesh(btag->mesh_base, NULL, NULL);
        if(btag->mesh_slot)
        {
            Render_Mesh(btag->mesh_slot, NULL, NULL);
        }
        if(btag->mesh_skin)
        {
            Render_SkinMesh(btag->mesh_skin, btag->transform);
        }
    }
}

void Render_Hair(struct entity_s *entity, const btScalar modelViewMatrix[16], const btScalar modelViewProjectionMatrix[16])
{
    if((!entity) || !(entity->character) || (entity->character->hair_count == 0) || !(entity->character->hairs))
        return;

    // Calculate lighting
    const lit_shader_description *shader = render_setupEntityLight(entity, modelViewMatrix);

    for(int h=0; h<entity->character->hair_count; h++)
    {
        for(uint16_t i=0; i<entity->character->hairs[h].element_count; i++)
        {
            btScalar subModelView[16];
            btScalar subModelViewProjection[16];

            btScalar transform[16];
            const btTransform &bt_tr = entity->character->hairs[h].elements[i].body->getWorldTransform();
            bt_tr.getOpenGLMatrix(transform);

            Mat4_Mat4_mul(subModelView, modelViewMatrix, transform);
            Mat4_Mat4_mul(subModelViewProjection, modelViewProjectionMatrix, transform);

            glUniformMatrix4fvARB(shader->model_view, 1, GL_FALSE, subModelView);
            glUniformMatrix4fvARB(shader->model_view_projection, 1, GL_FALSE, subModelViewProjection);
            Render_Mesh(entity->character->hairs[h].elements[i].mesh, NULL, NULL);
        }
    }
}

/**
 * drawing world models.
 */
void Render_Room(struct room_s *room, struct render_s *render, const btScalar modelViewMatrix[16], const btScalar modelViewProjectionMatrix[16])
{
    engine_container_p cont;
    entity_p ent;

    const shader_description *lastShader = 0;

#if STENCIL_FRUSTUM
    ////start test stencil test code
    bool need_stencil = false;
    if(room->frustum != NULL)
    {
        for(uint16_t i=0;i<room->overlapped_room_list_size;i++)
        {
            if(room->overlapped_room_list[i]->is_in_r_list)
            {
                need_stencil = true;
                break;
            }
        }

        if(need_stencil)
        {
            const int elem_size = (3 + 3 + 4 + 2) * sizeof(GLfloat);
            const unlit_tinted_shader_description *shader = render->shader_manager->getRoomShader(false, false);
            size_t buf_size;

            glUseProgramObjectARB(shader->program);
            glUniform1iARB(shader->sampler, 0);
            glUniformMatrix4fvARB(shader->model_view_projection, 1, false, engine_camera.gl_view_proj_mat);
            glEnable(GL_STENCIL_TEST);
            glClear(GL_STENCIL_BUFFER_BIT);
            glStencilFunc(GL_NEVER, 1, 0x00);
            glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);
            for(frustum_p f=room->frustum;f!=NULL;f=f->next)
            {
                buf_size = f->vertex_count * elem_size;
                GLfloat *v, *buf = (GLfloat*)Sys_GetTempMem(buf_size);
                v=buf;
                for(int16_t i=f->vertex_count-1;i>=0;i--)
                {
                    vec3_copy(v, f->vertex+3*i);                    v+=3;
                    vec3_copy_inv(v, engine_camera.view_dir);       v+=3;
                    vec4_set_one(v);                                v+=4;
                    v[0] = v[1] = 0.0;                              v+=2;
                }

                if(active_texture != renderer.world->textures[renderer.world->tex_count-1])
                {
                    active_texture = renderer.world->textures[renderer.world->tex_count-1];
                    glBindTexture(GL_TEXTURE_2D, active_texture);
                }
                glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
                glVertexPointer(3, GL_FLOAT, elem_size, buf+0);
                glNormalPointer(GL_FLOAT, elem_size, buf+3);
                glColorPointer(4, GL_FLOAT, elem_size, buf+3+3);
                glTexCoordPointer(2, GL_FLOAT, elem_size, buf+3+3+4);
                glDrawArrays(GL_TRIANGLE_FAN, 0, f->vertex_count);

                Sys_ReturnTempMem(buf_size);
            }
            glStencilFunc(GL_EQUAL, 1, 0xFF);
        }
    }
#endif

    if(!(renderer.style & R_SKIP_ROOM) && room->mesh)
    {
        btScalar modelViewProjectionTransform[16];
        Mat4_Mat4_mul(modelViewProjectionTransform, modelViewProjectionMatrix, room->transform);

        const unlit_tinted_shader_description *shader = render->shader_manager->getRoomShader(room->light_mode == 1, room->flags & 1);

        GLfloat tint[4];
        Render_CalculateWaterTint(tint, 1);
        if (shader != lastShader)
        {
            glUseProgramObjectARB(shader->program);
        }

        lastShader = shader;
        glUniform4fvARB(shader->tint_mult, 1, tint);
        glUniform1fARB(shader->current_tick, (GLfloat) SDL_GetTicks());
        glUniform1iARB(shader->sampler, 0);
        glUniformMatrix4fvARB(shader->model_view_projection, 1, false, modelViewProjectionTransform);
        Render_Mesh(room->mesh, NULL, NULL);
    }

    if (room->static_mesh_count > 0)
    {
        glUseProgramObjectARB(render->shader_manager->getStaticMeshShader()->program);
        for(uint32_t i=0; i<room->static_mesh_count; i++)
        {
            if(room->static_mesh[i].was_rendered || !Frustum_IsOBBVisibleInRoom(room->static_mesh[i].obb, room))
            {
                continue;
            }

            if((room->static_mesh[i].hide == 1) && !(renderer.style & R_DRAW_DUMMY_STATICS))
            {
                continue;
            }

            btScalar transform[16];
            Mat4_Mat4_mul(transform, modelViewProjectionMatrix, room->static_mesh[i].transform);
            glUniformMatrix4fvARB(render->shader_manager->getStaticMeshShader()->model_view_projection, 1, false, transform);
            base_mesh_s *mesh = room->static_mesh[i].mesh;
            GLfloat tint[4];

            vec4_copy(tint, room->static_mesh[i].tint);

            //If this static mesh is in a water room
            if(room->flags & TR_ROOM_FLAG_WATER)
            {
                Render_CalculateWaterTint(tint, 0);
            }
            glUniform4fvARB(render->shader_manager->getStaticMeshShader()->tint_mult, 1, tint);
            Render_Mesh(mesh, NULL, NULL);
            room->static_mesh[i].was_rendered = 1;
        }
    }

    if (room->containers)
    {
        for(cont=room->containers; cont; cont=cont->next)
        {
            switch(cont->object_type)
            {
            case OBJECT_ENTITY:
                ent = (entity_p)cont->object;
                if(ent->was_rendered == 0)
                {
                    if(Frustum_IsOBBVisibleInRoom(ent->obb, room))
                    {
                        Render_Entity(ent, modelViewMatrix, modelViewProjectionMatrix);
                    }
                    ent->was_rendered = 1;
                }
                break;
            };
        }
    }
#if STENCIL_FRUSTUM
    if(need_stencil)
    {
        glDisable(GL_STENCIL_TEST);
    }
#endif
}


void Render_Room_Sprites(struct room_s *room, struct render_s *render, const btScalar modelViewMatrix[16], const btScalar projectionMatrix[16])
{
    if (room->sprites_count > 0 && room->sprite_buffer)
    {
        const sprite_shader_description *shader = render->shader_manager->getSpriteShader();
        glUseProgramObjectARB(shader->program);
        glUniformMatrix4fvARB(shader->model_view, 1, GL_FALSE, modelViewMatrix);
        glUniformMatrix4fvARB(shader->projection, 1, GL_FALSE, projectionMatrix);
        glUniform1iARB(shader->sampler, 0);

        glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);

        glBindBufferARB(GL_ARRAY_BUFFER_ARB, room->sprite_buffer->array_buffer);

        glEnableVertexAttribArrayARB(sprite_shader_description::vertex_attribs::position);
        glVertexAttribPointerARB(sprite_shader_description::vertex_attribs::position, 3, GL_FLOAT, GL_FALSE, sizeof(GLfloat [7]), (const GLvoid *) sizeof(GLfloat [0]));

        glEnableVertexAttribArrayARB(sprite_shader_description::vertex_attribs::tex_coord);
        glVertexAttribPointerARB(sprite_shader_description::vertex_attribs::tex_coord, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat [7]), (const GLvoid *) sizeof(GLfloat [3]));

        glEnableVertexAttribArrayARB(sprite_shader_description::vertex_attribs::corner_offset);
        glVertexAttribPointerARB(sprite_shader_description::vertex_attribs::corner_offset, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat [7]), (const GLvoid *) sizeof(GLfloat [5]));

        glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, room->sprite_buffer->element_array_buffer);

        unsigned long offset = 0;
        for(uint32_t texture = 0; texture < room->sprite_buffer->num_texture_pages; texture++)
        {
            if(room->sprite_buffer->element_count_per_texture[texture] == 0)
            {
                continue;
            }

            if(active_texture != renderer.world->textures[texture])
            {
                active_texture = renderer.world->textures[texture];
                glBindTexture(GL_TEXTURE_2D, active_texture);
            }
            glDrawElements(GL_TRIANGLES, room->sprite_buffer->element_count_per_texture[texture], GL_UNSIGNED_SHORT, (GLvoid *) (offset * sizeof(uint16_t)));
            offset += room->sprite_buffer->element_count_per_texture[texture];
        }

        glDisableVertexAttribArrayARB(sprite_shader_description::vertex_attribs::position);
        glDisableVertexAttribArrayARB(sprite_shader_description::vertex_attribs::tex_coord);
        glDisableVertexAttribArrayARB(sprite_shader_description::vertex_attribs::corner_offset);
        glPopClientAttrib();
    }
}


/**
 * Безопасное добавление комнаты в список рендерера.
 * Если комната уже есть в списке - возвращается ноль и комната повторно не добавляется.
 * Если список полон, то ничего не добавляется
 */
int Render_AddRoom(struct room_s *room)
{
    int ret = 0;
    engine_container_p cont;
    btScalar dist, centre[3];

    if(room->is_in_r_list || !room->active)
    {
        return 0;
    }

    centre[0] = (room->bb_min[0] + room->bb_max[0]) / 2;
    centre[1] = (room->bb_min[1] + room->bb_max[1]) / 2;
    centre[2] = (room->bb_min[2] + room->bb_max[2]) / 2;
    dist = vec3_dist(renderer.cam->pos, centre);

    if(renderer.r_list_active_count < renderer.r_list_size)
    {
        renderer.r_list[renderer.r_list_active_count].room = room;
        renderer.r_list[renderer.r_list_active_count].active = 1;
        renderer.r_list[renderer.r_list_active_count].dist = dist;
        renderer.r_list_active_count++;
        ret++;

        if(room->flags & TR_ROOM_FLAG_SKYBOX)
            renderer.style |= R_DRAW_SKYBOX;
    }

    for(uint32_t i=0; i<room->static_mesh_count; i++)
    {
        room->static_mesh[i].was_rendered = 0;
        room->static_mesh[i].was_rendered_lines = 0;
    }

    for(cont=room->containers; cont; cont=cont->next)
    {
        switch(cont->object_type)
        {
        case OBJECT_ENTITY:
            ((entity_p)cont->object)->was_rendered = 0;
            ((entity_p)cont->object)->was_rendered_lines = 0;
            break;
        };
    }

    for(uint32_t i=0; i<room->sprites_count; i++)
    {
        room->sprites[i].was_rendered = 0;
    }

    room->is_in_r_list = 1;

    return ret;
}


void Render_CleanList()
{
    if(renderer.world->Character)
    {
        renderer.world->Character->was_rendered = 0;
        renderer.world->Character->was_rendered_lines = 0;
    }

    for(uint32_t i=0; i<renderer.r_list_active_count; i++)
    {
        renderer.r_list[i].active = 0;
        renderer.r_list[i].dist = 0.0;
        room_p r = renderer.r_list[i].room;
        renderer.r_list[i].room = NULL;

        r->is_in_r_list = 0;
        r->active_frustums = 0;
        r->frustum = NULL;
    }

    renderer.style &= ~R_DRAW_SKYBOX;
    renderer.r_list_active_count = 0;
}

/**
 * Render all visible rooms
 */
void Render_DrawList()
{
    if(!renderer.world)
    {
        return;
    }

    if(renderer.style & R_DRAW_WIRE)
    {
        glPolygonMode(GL_FRONT, GL_LINE);
    }
    else if(renderer.style & R_DRAW_POINTS)
    {
        glEnable(GL_POINT_SMOOTH);
        glPointSize(4);
        glPolygonMode(GL_FRONT, GL_POINT);
    }
    else
    {
        glPolygonMode(GL_FRONT, GL_FILL);
    }

    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);

    active_texture = 0;
    Render_SkyBox(renderer.cam->gl_view_proj_mat);

    if(renderer.world->Character)
    {
        Render_Entity(renderer.world->Character, renderer.cam->gl_view_mat, renderer.cam->gl_view_proj_mat);
        Render_Hair(renderer.world->Character, renderer.cam->gl_view_mat, renderer.cam->gl_view_proj_mat);
    }

    /*
     * room rendering
     */
    for(uint32_t i=0; i<renderer.r_list_active_count; i++)
    {
        Render_Room(renderer.r_list[i].room, &renderer, renderer.cam->gl_view_mat, renderer.cam->gl_view_proj_mat);
    }

    glDisable(GL_CULL_FACE);
    glDisableClientState(GL_NORMAL_ARRAY);                                      ///@FIXME: reduce number of gl state changes
    for(uint32_t i=0; i<renderer.r_list_active_count; i++)
    {
        Render_Room_Sprites(renderer.r_list[i].room, &renderer, renderer.cam->gl_view_mat, renderer.cam->gl_proj_mat);
    }
    glEnableClientState(GL_NORMAL_ARRAY);

    /*
     * NOW render transparency polygons
     */
    /*First generate BSP from base room mesh - it has good for start splitter polygons*/
    for(uint32_t i=0;i<renderer.r_list_active_count;i++)
    {
        room_p r = renderer.r_list[i].room;
        if((r->mesh != NULL) && (r->mesh->transparency_polygons != NULL))
        {
            render_dBSP.addNewPolygonList(r->mesh->transparency_polygons, r->transform, renderer.cam->frustum);
        }
    }

    for(uint32_t i=0;i<renderer.r_list_active_count;i++)
    {
        room_p r = renderer.r_list[i].room;
        // Add transparency polygons from static meshes (if they exists)
        for(uint16_t j=0;j<r->static_mesh_count;j++)
        {
            if((r->static_mesh[j].mesh->transparency_polygons != NULL) && Frustum_IsOBBVisibleInRoom(r->static_mesh[j].obb, r))
            {
                render_dBSP.addNewPolygonList(r->static_mesh[j].mesh->transparency_polygons, r->static_mesh[j].transform, renderer.cam->frustum);
            }
        }

        // Add transparency polygons from all entities (if they exists) // yes, entities may be animated and intersects with each others;
        for(engine_container_p cont=r->containers;cont!=NULL;cont=cont->next)
        {
            if(cont->object_type == OBJECT_ENTITY)
            {
                entity_p ent = (entity_p)cont->object;
                if((ent->bf.animations.model->transparency_flags == MESH_HAS_TRANSPARENCY) && (ent->state_flags & ENTITY_STATE_VISIBLE) && (Frustum_IsOBBVisibleInRoom(ent->obb, r)))
                {
                    btScalar tr[16];
                    for(uint16_t j=0;j<ent->bf.bone_tag_count;j++)
                    {
                        if(ent->bf.bone_tags[j].mesh_base->transparency_polygons != NULL)
                        {
                            Mat4_Mat4_mul(tr, ent->transform, ent->bf.bone_tags[j].full_transform);
                            render_dBSP.addNewPolygonList(ent->bf.bone_tags[j].mesh_base->transparency_polygons, tr, renderer.cam->frustum);
                        }
                    }
                }
            }
        }
    }

    if((engine_world.Character != NULL) && (engine_world.Character->bf.animations.model->transparency_flags == MESH_HAS_TRANSPARENCY))
    {
        btScalar tr[16];
        entity_p ent = engine_world.Character;
        for(uint16_t j=0;j<ent->bf.bone_tag_count;j++)
        {
            if(ent->bf.bone_tags[j].mesh_base->transparency_polygons != NULL)
            {
                Mat4_Mat4_mul(tr, ent->transform, ent->bf.bone_tags[j].full_transform);
                render_dBSP.addNewPolygonList(ent->bf.bone_tags[j].mesh_base->transparency_polygons, tr, renderer.cam->frustum);
            }
        }
    }

    if((render_dBSP.m_root->polygons_front != NULL) && (render_dBSP.m_vbo != 0))
    {
        const unlit_tinted_shader_description *shader = renderer.shader_manager->getRoomShader(false, false);
        glUseProgramObjectARB(shader->program);
        glUniform1iARB(shader->sampler, 0);
        glUniformMatrix4fvARB(shader->model_view_projection, 1, false, renderer.cam->gl_view_proj_mat);
        glDepthMask(GL_FALSE);
        glDisable(GL_ALPHA_TEST);
        glEnable(GL_BLEND);
        active_transparency = 0;
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, render_dBSP.m_vbo);
        glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
        glBufferDataARB(GL_ARRAY_BUFFER_ARB, render_dBSP.getActiveVertexCount() * sizeof(vertex_t), render_dBSP.getVertexArray(), GL_DYNAMIC_DRAW);
        glVertexPointer(3, GL_BT_SCALAR, sizeof(vertex_t), (void*)offsetof(vertex_t, position));
        glColorPointer(4, GL_FLOAT, sizeof(vertex_t), (void*)offsetof(vertex_t, color));
        glNormalPointer(GL_FLOAT, sizeof(vertex_t), (void*)offsetof(vertex_t, normal));
        glTexCoordPointer(2, GL_FLOAT, sizeof(vertex_t), (void*)offsetof(vertex_t, tex_coord));
        Render_BSPBackToFront(render_dBSP.m_root);
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
    //Reset polygon draw mode
    glPolygonMode(GL_FRONT, GL_FILL);
}

void Render_DrawList_DebugLines()
{
    if (!renderer.world || !(renderer.style & (R_DRAW_BOXES | R_DRAW_ROOMBOXES | R_DRAW_PORTALS | R_DRAW_FRUSTUMS | R_DRAW_AXIS | R_DRAW_NORMALS | R_DRAW_COLL)))
    {
        return;
    }

    if(renderer.world->Character)
    {
        debugDrawer.drawEntityDebugLines(renderer.world->Character);
    }

    /*
     * Render world debug information
     */
    if((renderer.style & R_DRAW_NORMALS) && (renderer.world != NULL) && (renderer.world->sky_box != NULL))
    {
        GLfloat tr[16];
        btScalar *p;
        Mat4_E_macro(tr);
        p = renderer.world->sky_box->animations->frames->bone_tags->offset;
        vec3_add(tr+12, renderer.cam->pos, p);
        p = renderer.world->sky_box->animations->frames->bone_tags->qrotate;
        Mat4_set_qrotation(tr, p);
        debugDrawer.drawMeshDebugLines(renderer.world->sky_box->mesh_tree->mesh_base, tr, NULL, NULL);
    }

    for(uint32_t i=0; i<renderer.r_list_active_count; i++)
    {
        debugDrawer.drawRoomDebugLines(renderer.r_list[i].room, &renderer);
    }

    if(renderer.style & R_DRAW_COLL)
    {
        bt_engine_dynamicsWorld->debugDrawWorld();
    }

    if(!debugDrawer.IsEmpty())
    {
        const unlit_tinted_shader_description *shader = renderer.shader_manager->getRoomShader(false, false);
        glUseProgramObjectARB(shader->program);
        glUniform1iARB(shader->sampler, 0);
        glUniformMatrix4fvARB(shader->model_view_projection, 1, false, renderer.cam->gl_view_proj_mat);
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
        if(active_texture != renderer.world->textures[renderer.world->tex_count-1])
        {
            active_texture = renderer.world->textures[renderer.world->tex_count-1];
            glBindTexture(GL_TEXTURE_2D, active_texture);
        }
        glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
        glPointSize( 6.0f );
        glLineWidth( 3.0f );
        debugDrawer.render();
    }
}

/**
 * The reccursion algorithm: go through the rooms with portal - frustum occlusion test
 * @portal - we entered to the room through that portal
 * @frus - frustum that intersects the portal
 * @return number of added rooms
 */
int Render_ProcessRoom(struct portal_s *portal, struct frustum_s *frus)
{
    int ret = 0;
    room_p room = portal->dest_room;                                            // куда ведет портал
    room_p src_room = portal->current_room;                                     // откуда ведет портал
    portal_p p;                                                                 // указатель на массив порталов входной ф-ии
    frustum_p gen_frus;                                                         // новый генерируемый фрустум

    if((src_room == NULL) || !src_room->active || (room == NULL) || !room->active)
    {
        return 0;
    }

    p = room->portals;

    for(uint16_t i=0; i<room->portal_count; i++,p++)                            // перебираем все порталы входной комнаты
    {
        if((p->dest_room->active) && (p->dest_room != src_room))                // обратно идти даже не пытаемся
        {
            gen_frus = engine_frustumManager.portalFrustumIntersect(p, frus, &renderer);             // Главная ф-я портального рендерера. Тут и проверка
            if(NULL != gen_frus)                                                // на пересечение и генерация фрустума по порталу
            {
                ret++;
                Render_AddRoom(p->dest_room);
                Render_ProcessRoom(p, gen_frus);
            }
        }
    }
    return ret;
}

/**
 * Renderer list generation by current world and camera
 */
void Render_GenWorldList()
{
    if(renderer.world == NULL)
    {
        return;
    }

    Render_CleanList();                                                         // clear old render list
    debugDrawer.reset();
    render_dBSP.reset(engine_world.anim_sequences);
    engine_frustumManager.reset();
    renderer.cam->frustum->next = NULL;

    room_p curr_room = Room_FindPosCogerrence(renderer.cam->pos, renderer.cam->current_room);                // find room that contains camera

    renderer.cam->current_room = curr_room;                                     // set camera's cuttent room pointer
    if(curr_room != NULL)                                                       // camera located in some room
    {
        curr_room->frustum = NULL;                                              // room with camera inside has no frustums!
        curr_room->max_path = 0;
        Render_AddRoom(curr_room);                                              // room with camera inside adds to the render list immediately
        portal_p p = curr_room->portals;                                        // pointer to the portals array
        for(uint16_t i=0; i<curr_room->portal_count; i++,p++)                   // go through all start room portals
        {
            frustum_p last_frus = engine_frustumManager.portalFrustumIntersect(p, renderer.cam->frustum, &renderer);
            if(last_frus)
            {
                Render_AddRoom(p->dest_room);                                   // portal destination room
                last_frus->parents_count = 1;                                   // created by camera
                Render_ProcessRoom(p, last_frus);                               // next start reccursion algorithm
            }
        }
    }
    else                                                                        // camera is out of all rooms
    {
        curr_room = renderer.world->rooms;                                      // draw full level. Yes - it is slow, but it is not gameplay - it is debug.
        for(uint32_t i=0; i<renderer.world->room_count; i++,curr_room++)
        {
            if(Frustum_IsAABBVisible(curr_room->bb_min, curr_room->bb_max, renderer.cam->frustum))
            {
                Render_AddRoom(curr_room);
            }
        }
    }
}

/**
 * Состыковка рендерера и "мира"
 */
void Render_SetWorld(struct world_s *world)
{
    uint32_t list_size = world->room_count + 128;                               // magick 128 was added for debug and testing

    if(renderer.world)
    {
        if(renderer.r_list_size < list_size)                                    // if old list less than new one requiring
        {
            renderer.r_list = (render_list_p)realloc(renderer.r_list, list_size * sizeof(render_list_t));
            for(uint32_t i=0; i<list_size; i++)
            {
                renderer.r_list[i].active = 0;
                renderer.r_list[i].room = NULL;
                renderer.r_list[i].dist = 0.0;
            }
        }
    }
    else
    {
        renderer.r_list = Render_CreateRoomListArray(list_size);
    }

    renderer.world = world;
    renderer.style &= ~R_DRAW_SKYBOX;
    renderer.r_list_size = list_size;
    renderer.r_list_active_count = 0;

    renderer.cam = &engine_camera;
    engine_camera.frustum->next = NULL;
    engine_camera.current_room = NULL;

    for(uint32_t i=0; i<world->room_count; i++)
    {
        world->rooms[i].is_in_r_list = 0;
    }
}


void Render_CalculateWaterTint(GLfloat *tint, uint8_t fixed_colour)
{
    if(engine_world.version < TR_IV)  // If water room and level is TR1-3
    {
        if(engine_world.version < TR_III)
        {
             // Placeholder, color very similar to TR1 PSX ver.
            if(fixed_colour > 0)
            {
                tint[0] = 0.585f;
                tint[1] = 0.9f;
                tint[2] = 0.9f;
                tint[3] = 1.0f;
            }
            else
            {
                tint[0] *= 0.585f;
                tint[1] *= 0.9f;
                tint[2] *= 0.9f;
            }
        }
        else
        {
            // TOMB3 - closely matches TOMB3
            if(fixed_colour > 0)
            {
                tint[0] = 0.275f;
                tint[1] = 0.45f;
                tint[2] = 0.5f;
                tint[3] = 1.0f;
            }
            else
            {
                tint[0] *= 0.275f;
                tint[1] *= 0.45f;
                tint[2] *= 0.5f;
            }
        }
    }
    else
    {
        if(fixed_colour > 0)
        {
            tint[0] = 1.0f;
            tint[1] = 1.0f;
            tint[2] = 1.0f;
            tint[3] = 1.0f;
        }
    }
}
