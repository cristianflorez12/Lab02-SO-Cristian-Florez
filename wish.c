#include <stdio.h>      /* printf, fprintf, fgets, getline */
#include <stdlib.h>     /* exit, malloc, free, realloc */
#include <string.h>     /* strsep, strcmp, strlen, strdup, strcpy, strcat */
#include <unistd.h>     /* fork, execv, chdir, access, write, close, dup2 */
#include <sys/wait.h>   /* wait, waitpid */
#include <fcntl.h>      /* open, O_WRONLY, O_CREAT, O_TRUNC */

/* CONSTANTES Y VARIABLES GLOBALES */

/* Mensaje de error estándar
 * Se imprime a stderr usando write() para cualquier tipo de error */
char error_message[30] = "An error has occurred\n";

/* Arreglo de punteros que almacena los directorios del search path
 * El path inicial contiene únicamente /bin
 * Se termina con NULL para poder iterar fácilmente sobre él */
char **search_path = NULL;

/* Número actual de directorios en el search path */
int path_count = 0;

/* FUNCIÓN: print_error
 * Imprime el mensaje de error estándar a stderr
 * Según el enunciado, siempre se usa el mismo mensaje de error */
void print_error() {
    /* write() escribe directamente a STDERR_FILENO (descriptor 2) */
    write(STDERR_FILENO, error_message, strlen(error_message));
}

/* FUNCIÓN: init_path
 * Inicializa el search path con el directorio /bin */
void init_path() {
    /* Reservar espacio para un puntero (más el NULL terminador) */
    search_path = malloc(2 * sizeof(char *));
    if (!search_path) {
        /* Si malloc falla, no podemos continuar */
        print_error();
        exit(1);
    }
    /* Copiar la cadena "/bin" en el primer slot del arreglo */
    search_path[0] = strdup("/bin");
    /* Marcar el fin del arreglo con NULL */
    search_path[1] = NULL;
    /* Actualizar el contador de directorios */
    path_count = 1;
}

/* FUNCIÓN: free_path
 * Libera la memoria ocupada por el search path actual
 * Es llamada antes de actualizar el path con el built-in 'route' */
void free_path() {
    /* Recorrer cada entrada del path y liberar la cadena */
    for (int i = 0; i < path_count; i++) {
        free(search_path[i]); /* Liberar la cadena duplicada con strdup */
    }
    /* Liberar el arreglo de punteros */
    free(search_path);
    search_path = NULL;
    path_count = 0;
}

/* FUNCIÓN: find_executable
 * Busca un ejecutable en los directorios del search path
 * Parámetros: cmd - Nombre del comando a buscar (ej: "ls")
 * Retorna:
 *   Puntero a string con la ruta completa si se encontró (ej: "/bin/ls")
 *   NULL si no se encontró en ningún directorio del path */
char *find_executable(char *cmd) {
    /* Buffer que contendrá la ruta completa: directorio + "/" + comando */
    char full_path[1024];

    /* Recorrer cada directorio en el search path */
    for (int i = 0; i < path_count; i++) {
        /* Construir la ruta completa concatenando: directorio + "/" + comando
         * Ejemplo: "/bin" + "/" + "ls" = "/bin/ls" */
        strcpy(full_path, search_path[i]);
        strcat(full_path, "/");
        strcat(full_path, cmd);

        /* access(ruta, X_OK) retorna 0 si el archivo existe y es ejecutable */
        if (access(full_path, X_OK) == 0) {
            /* Encontrado: retornar una copia de la ruta completa */
            return strdup(full_path);
        }
    }

    /* El ejecutable no se encontró en ningún directorio del path */
    return NULL;
}

/* FUNCIÓN: builtin_exit
 * Implementación del comando integrado 'exit'
 * Parámetros: args - Arreglo de argumentos del comando
 *             argc - Número de argumentos
 * Retorna: void */
void builtin_exit(char **args, int argc) {
    /* Si se pasó algún argumento adicional, es un error */
    if (argc > 1) {
        print_error();
        return; /* No salir, solo reportar el error y continuar */
    }
    /* Salir con código 0 (éxito) */
    exit(0);
}

/* FUNCIÓN: builtin_chd
 * Implementación del comando integrado 'chd' (change directory)
 * Parámetros: args - Arreglo de argumentos (args[1] es el directorio)
 *             argc - Número de argumentos (debe ser exactamente 2: "chd" + directorio)
 * Retorna: void */
void builtin_chd(char **args, int argc) {
    /* Verificar que se haya dado exactamente un argumento de directorio */
    if (argc != 2) {
        print_error();
        return;
    }

    /* chdir() cambia el directorio de trabajo actual del proceso
     * Si falla (retorna -1), el directorio no existe o no hay permisos */
    if (chdir(args[1]) != 0) {
        print_error();
    }
}

/* FUNCIÓN: builtin_route
 * Implementación del comando integrado 'route'
 * Parámetros: args - Arreglo de argumentos (args[1..argc-1] son los directorios)
 *             argc - Número de argumentos
 * Retorna: void */
void builtin_route(char **args, int argc) {
    /* Liberar el search path anterior para evitar memory leaks */
    free_path();

    /* Calcular cuántos nuevos directorios se van a agregar */
    int new_count = argc - 1; /* args[0] es "route", los demás son directorios */

    /* Reservar espacio para el nuevo arreglo de punteros + NULL terminador */
    search_path = malloc((new_count + 1) * sizeof(char *));
    if (!search_path) {
        print_error();
        return;
    }

    /* Copiar cada directorio nuevo al search path */
    for (int i = 0; i < new_count; i++) {
        search_path[i] = strdup(args[i + 1]); /* args[1], args[2], etc. */
    }
    /* Marcar el fin del arreglo */
    search_path[new_count] = NULL;

    /* Actualizar el contador global */
    path_count = new_count;
}

/* ESTRUCTURA: Command
 * Representa un único comando a ejecutar, incluyendo:
 *   - Sus argumentos (args[])
 *   - El número de argumentos (argc)
 *   - El archivo de redirección de salida (si aplica)
 *   - Indicador de si tiene redirección (has_redirect)
 */
typedef struct {
    char **args;         /* Arreglo de strings: args[0] = comando, args[1..] = argumentos */
    int argc;            /* Cantidad total de strings en args */
    char *redirect_file; /* Nombre del archivo de salida si hay '>' */
    int has_redirect;    /* 1 si hay redirección de salida, 0 si no */
} Command;

/* FUNCIÓN: parse_single_command
 * Analiza (parsea) una línea de texto que contiene UN comando
 * (posiblemente con argumentos y redirección), y lo convierte
 * en una estructura Command.
 *
 * Detecta el operador '>' para la redirección de salida.
 * Valida que no haya múltiples operadores de redirección ni
 * múltiples archivos de destino.
 *
 * Parámetros:
 *   line  - String con el comando a parsear (es modificado por strsep)
 *
 * Retorna:
 *   Puntero a Command inicializado, o NULL si hay error de parseo.
 *   La memoria debe liberarse por el llamador con free_command().
 */
Command *parse_single_command(char *line) {
    /* Reservar memoria para la estructura Command */
    Command *cmd = malloc(sizeof(Command));
    if (!cmd) { print_error(); return NULL; }

    /* Inicializar los campos del comando */
    cmd->args = malloc(128 * sizeof(char *)); /* Máximo 128 argumentos */
    if (!cmd->args) { free(cmd); print_error(); return NULL; }
    cmd->argc = 0;
    cmd->redirect_file = NULL;
    cmd->has_redirect = 0;

    /* Variables para controlar el estado del parseo de redirección */
    int redirect_count = 0; /* Cuántos '>' se han visto */
    int after_redirect = 0; /* Cuántos tokens vienen después del '>' */

    /* Puntero auxiliar para strsep - debe apuntar al inicio de la línea */
    char *ptr = line;
    char *token;

    /* strsep() extrae tokens delimitados por espacio o tabulación.
     * Modifica la cadena original reemplazando el delimitador con '\0'.
     * Se llama en bucle hasta que retorna NULL (fin de cadena). */
    while ((token = strsep(&ptr, " \t")) != NULL) {
        /* Ignorar tokens vacíos (causados por espacios múltiples) */
        if (strlen(token) == 0) continue;

        /* Verificar si el token es el operador de redirección '>' */
        if (strcmp(token, ">") == 0) {
            redirect_count++;
            /* Más de un operador de redirección es error según el enunciado */
            if (redirect_count > 1) {
                print_error();
                free(cmd->args);
                free(cmd);
                return NULL;
            }
            cmd->has_redirect = 1;
            continue; /* El siguiente token será el nombre del archivo */
        }

        /* Si ya se vio '>', el próximo token es el archivo de destino */
        if (cmd->has_redirect && redirect_count == 1) {
            after_redirect++;
            /* Más de un archivo después del '>' es error */
            if (after_redirect > 1) {
                print_error();
                free(cmd->args);
                free(cmd);
                return NULL;
            }
            cmd->redirect_file = strdup(token);
            continue;
        }

        /* Agregar el token como argumento del comando */
        cmd->args[cmd->argc] = strdup(token);
        cmd->argc++;
    }

    /* Validar: si hay '>' pero no hay archivo destino, es error */
    if (cmd->has_redirect && cmd->redirect_file == NULL) {
        print_error();
        free(cmd->args);
        free(cmd);
        return NULL;
    }

    /* Terminar el arreglo de argumentos con NULL (requerido por execv) */
    cmd->args[cmd->argc] = NULL;

    return cmd;
}

/* FUNCIÓN: free_command
 * Libera la memoria asociada a una estructura Command.
 * Parámetros: cmd - Puntero al Command a liberar
 */
void free_command(Command *cmd) {
    if (!cmd) return;
    /* Liberar cada string duplicado en el arreglo de argumentos */
    for (int i = 0; i < cmd->argc; i++) {
        free(cmd->args[i]);
    }
    free(cmd->args);
    /* Liberar el nombre del archivo de redirección si existe */
    if (cmd->redirect_file) free(cmd->redirect_file);
    free(cmd);
}

/* FUNCIÓN: execute_command
 *
 * Si el comando es un built-in (exit, chd, route), lo ejecuta
 * directamente sin usar fork()/execv().
 *
 * Si es un comando externo, usa el patrón fork()+execv():
 *   1. fork() crea un proceso hijo.
 *   2. En el hijo: opcionalmente redirige stdout/stderr, luego execv().
 *   3. En el padre: retorna el PID del hijo para que el llamador
 *      pueda hacer waitpid() cuando sea necesario.
 *
 * Parámetros:
 *   cmd         - Estructura Command a ejecutar
 *   wait_child  - Si 1, el padre espera al hijo con waitpid()
 *                 Si 0, retorna inmediatamente (para ejecución paralela)
 *
 * Retorna:
 *   PID del proceso hijo si wait_child == 0
 *   -1 si el comando es built-in o si hubo error
 * ========================================================= */
int execute_command(Command *cmd, int wait_child) {
    /* Ignorar comandos vacíos (línea en blanco) */
    if (cmd->argc == 0) return -1;

    /* Detección y ejecución de built-in commands */

    /* Built-in: exit */
    if (strcmp(cmd->args[0], "exit") == 0) {
        builtin_exit(cmd->args, cmd->argc);
        return -1;
    }

    /* Built-in: chd (change directory) */
    if (strcmp(cmd->args[0], "chd") == 0) {
        builtin_chd(cmd->args, cmd->argc);
        return -1;
    }

    /* Built-in: route (modifica el search path) */
    if (strcmp(cmd->args[0], "route") == 0) {
        builtin_route(cmd->args, cmd->argc);
        return -1;
    }

    /* Ejecución de comandos externos */
    /* Si el path está vacío, no se puede ejecutar ningún programa externo */
    if (path_count == 0) {
        print_error();
        return -1;
    }

    /* Buscar el ejecutable en el search path */
    char *exec_path = find_executable(cmd->args[0]);
    if (exec_path == NULL) {
        /* El ejecutable no existe en ningún directorio del path */
        print_error();
        return -1;
    }

    /* fork() crea un proceso hijo
     * Retorna: 0 en el proceso hijo, PID del hijo en el padre, -1 en error */
    pid_t pid = fork();

    if (pid < 0) {
        /* Error al crear el proceso hijo */
        print_error();
        free(exec_path);
        return -1;
    }

    if (pid == 0) {
        /* Código del proceso hijo */

        /* Manejar la redirección de salida si el comando tiene '>' */
        if (cmd->has_redirect) {
            /* Abrir el archivo de destino:
             *   O_WRONLY: Solo escritura
             *   O_CREAT:  Crear si no existe
             *   O_TRUNC:  Truncar (vaciar) si ya existe
             *   0644:     Permisos del archivo: rw-r--r-- */
            int fd = open(cmd->redirect_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                print_error();
                exit(1);
            }

            /* dup2(fd, STDOUT_FILENO): redirige stdout (1) al archivo
             * A partir de aquí, printf y write(1,...) van al archivo. */
            dup2(fd, STDOUT_FILENO);

            /* dup2(fd, STDERR_FILENO): redirige stderr (2) al archivo también*/
            dup2(fd, STDERR_FILENO);

            /* Ya no necesitamos el descriptor original del archivo */
            close(fd);
        }

        /* execv(path, args):
         *   - path: ruta completa al ejecutable (ej: "/bin/ls")
         *   - args: arreglo de strings con argumentos, terminado en NULL
         *   Si tiene éxito, NO retorna (reemplaza la imagen del proceso)
         *   Si retorna, ocurrió un error */
        execv(exec_path, cmd->args);

        /* Si llegamos aquí, execv() falló */
        print_error();
        exit(1); /* Terminar el proceso hijo con error */
    }

    /* Código del proceso padre */
    free(exec_path); /* Liberar la memoria de la ruta encontrada */

    if (wait_child) {
        /* Esperar a que el proceso hijo termine
         * waitpid(pid, NULL, 0): espera específicamente al hijo con PID dado */
        waitpid(pid, NULL, 0);
        return -1;
    } else {
        /* Retornar el PID para que el llamador espere luego (comandos paralelos) */
        return pid;
    }
}

/* FUNCIÓN: run_line
 * Procesa una línea de entrada completa
 *
 * Maneja el operador '&' para comandos paralelos:
 *   1. Divide la línea en subcadenas por '&'.
 *   2. Parsea cada subcadena como un Command independiente.
 *   3. Si hay un solo comando, lo ejecuta y espera.
 *   4. Si hay múltiples comandos (paralelos), lanza todos con fork()
 *      sin esperar, y luego hace waitpid() de todos al final.
 *
 * Parámetros:
 *   line  - String con la línea completa a procesar
 */
void run_line(char *line) {
    /* Eliminar el salto de línea al final si existe */
    line[strcspn(line, "\n")] = '\0';

    /* Arreglo para guardar los comandos paralelos */
    Command *commands[128]; /* Máximo 128 comandos en paralelo */
    int num_commands = 0;

    /* Puntero auxiliar para strsep sobre & */
    char *ptr = line;
    char *segment;

    /* Dividir la línea por & para obtener cada comando paralelo */
    while ((segment = strsep(&ptr, "&")) != NULL) {
        /* Ignorar segmentos completamente vacíos */
        if (strlen(segment) == 0) continue;

        /* Verificar que el segmento tenga contenido real (no solo espacios) */
        int has_content = 0;
        for (int i = 0; segment[i] != '\0'; i++) {
            if (segment[i] != ' ' && segment[i] != '\t') {
                has_content = 1;
                break;
            }
        }
        if (!has_content) continue;

        /* Parsear el segmento como un comando individual */
        Command *cmd = parse_single_command(segment);
        if (cmd != NULL) {
            commands[num_commands++] = cmd;
        }
    }

    /* Si no se obtuvo ningún comando válido, no hacer nada */
    if (num_commands == 0) return;

    /* Caso 1: Un solo comando. Ejecutar y esperar */
    if (num_commands == 1) {
        execute_command(commands[0], 1); /* wait_child = 1 */
        free_command(commands[0]);
        return;
    }

    /* Caso 2: Múltiples comandos paralelos */

    /* Arreglo para almacenar los PIDs de los hijos lanzados */
    pid_t pids[128];
    int launched = 0;

    /* Lanzar todos los comandos sin esperar (wait_child = 0) */
    for (int i = 0; i < num_commands; i++) {
        int pid = execute_command(commands[i], 0); /* No esperar */
        if (pid > 0) {
            pids[launched++] = pid; /* Guardar el PID para esperar luego */
        }
        free_command(commands[i]);
    }

    /* Esperar a que TODOS los procesos hijos terminen */
    for (int i = 0; i < launched; i++) {
        waitpid(pids[i], NULL, 0);
    }
}

/* FUNCIÓN: main
 * Punto de entrada del shell wish
 * Maneja dos modos de operación:
 *   - Modo interactivo (sin argumentos): lee de stdin, muestra "wish"
 *   - Modo batch (un argumento): lee comandos desde un archivo
 * Más de un argumento genera error y exit(1)
 * EOF en cualquier modo invoca exit(0)
 */
int main(int argc, char *argv[]) {
    /* Inicializar el search path con /bin (según el enunciado) */
    init_path();

    /* Determinar el modo de operación */
    FILE *input = NULL;     /* Fuente de entrada de comandos */
    int batch_mode = 0;     /* 0 = interactivo, 1 = batch */

    if (argc == 1) {
        /* Sin argumentos: modo interactivo, leer de stdin */
        input = stdin;
        batch_mode = 0;
    } else if (argc == 2) {
        /* Un argumento: modo batch, abrir el archivo indicado */
        input = fopen(argv[1], "r");
        if (input == NULL) {
            /* El archivo no pudo abrirse: error fatal según el enunciado */
            print_error();
            exit(1);
        }
        batch_mode = 1;
    } else {
        /* Más de un argumento: error fatal */
        print_error();
        exit(1);
    }

    /* Buffer para almacenar la línea leída
     * Se usan variables para getline() que maneja memoria dinámica */
    char *line = NULL;      /* getline() reserva la memoria necesaria */
    size_t line_len = 0;    /* Tamaño del buffer (gestionado por getline) */
    ssize_t nread;          /* Número de caracteres leídos por getline */

    /* Bucle principal del shell */
    /* El shell corre indefinidamente hasta que:
     *   1. El usuario escribe 'exit'
     *   2. Se encuentra EOF (fin de archivo) */
    while (1) {
        /* En modo interactivo, mostrar el prompt
         * En modo batch, NO se muestra el prompt */
        if (!batch_mode) {
            /* El prompt es "wish> " */
            printf("wish> ");
            fflush(stdout); /* Forzar la impresión inmediata del prompt */
        }

        /* getline() lee una línea completa:
         *   - &line: puntero al buffer (puede ser reasignado por getline)
         *   - &line_len: tamaño del buffer
         *   - input: el FILE* desde donde leer
         * Retorna el número de caracteres leídos, o -1 en EOF/error. */
        nread = getline(&line, &line_len, input);

        if (nread == -1) {
            /* EOF encontrado: salir con exit(0) según el enunciado */
            free(line);
            if (batch_mode) fclose(input);
            free_path();
            exit(0);
        }

        /* Procesar la línea leída */
        run_line(line);
    }

    /* Este punto nunca se alcanza (el bucle termina con exit()),
     * pero es buena práctica liberar recursos */
    free(line);
    if (batch_mode) fclose(input);
    free_path();

    return 0;
}